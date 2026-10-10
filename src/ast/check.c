/* check.c — the AST structural check (§10): rules 1–6 and 8 (rule 7 is check_elab.c). */
#include "ast/check.h"

#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SHOW_PER_RULE = 10, /* violations logged per rule; the rest are summed */
    REF_SPAN = 1,       /* rule 3: referenced by a child span */
    REF_ATTR = 2,       /* rule 3: referenced by the attribute map */
    ATTR_COUNT_SHIFT = 32,
    RULE_KIND = 1,
    RULE_SLOTS = 2,
    RULE_REFS = 3,
    RULE_NAME = 4,
    RULE_HEIGHT = 5,
    RULE_TABLES = 6,
    RULE_SYMTAB = 8,
};

/* --- reporting ----------------------------------------------------------------------------- */

void odin3_ast_check_fail(odin3_ast_checker *cx, odin3_ast_check_site site, const char *what) {
    if (++cx->errors[site.rule] <= SHOW_PER_RULE) {
        odin3_log(ODIN3_LOG_ERROR, "ast check: rule %" PRIu32 ": %s %" PRIu32 ": %s", site.rule,
                  site.label, site.id, what);
    }
}

/* A node's label: its kind name, "node" for an unknown kind. */
static const char *label_of(const odin3_ast_node *rec) {
    const odin3_ast_kind_info *info = rec != NULL ? odin3_ast_kind_info_of(rec->kind) : NULL;
    return info != NULL ? info->name : "node";
}

void odin3_ast_check_node_fail(odin3_ast_checker *cx, uint32_t rule, const char *what) {
    odin3_ast_check_site site = {rule, label_of(cx->rec), cx->id.v};
    odin3_ast_check_fail(cx, site, what);
}

/* --- safe child access --------------------------------------------------------------------- */

/* True when rec's span lies inside the child table. */
static bool span_in_table(const odin3_ast *ast, const odin3_ast_node *rec) {
    return (uint64_t)rec->child + rec->nchild <= ast->children.len;
}

odin3_ast_span odin3_ast_check_span(const odin3_ast_checker *cx, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(cx->ast, node);
    const odin3_ast_kind_info *info = rec != NULL ? odin3_ast_kind_info_of(rec->kind) : NULL;
    if (info == NULL || info->payload != ODIN3_AST_PAYLOAD_NONE || rec->nchild == 0 ||
        !span_in_table(cx->ast, rec)) {
        return (odin3_ast_span){NULL, 0};
    }
    return (odin3_ast_span){(const odin3_ast_id *)cx->ast->children.data + rec->child, rec->nchild};
}

odin3_ast_id odin3_ast_check_kid(const odin3_ast_checker *cx, odin3_ast_id node, uint32_t idx) {
    odin3_ast_span kids = odin3_ast_check_span(cx, node);
    return idx < kids.n ? kids.ids[idx] : (odin3_ast_id){0};
}

static odin3_ast_kind kind_at(const odin3_ast_checker *cx, odin3_ast_id node) {
    return odin3_ast_kind_of(cx->ast, node);
}

/* --- rule 1: kind, sub, flags, payload, locations ------------------------------------------ */

/* A flag field and its largest value. */
typedef struct field_rule {
    uint8_t kind;
    uint8_t shift;
    uint16_t mask;
    uint16_t max;
} field_rule;

static const field_rule FIELDS[] = {
    {ODIN3_AST_PORT_DECL, ODIN3_AST_F_PORT_DECL_DT_SHIFT, ODIN3_AST_F_PORT_DECL_DT_MASK,
     ODIN3_AST_DT_TIME},
    {ODIN3_AST_PORT_DECL, ODIN3_AST_F_PORT_DECL_NET_SHIFT, ODIN3_AST_F_PORT_DECL_NET_MASK,
     ODIN3_AST_NET_DECL_COUNT},
    {ODIN3_AST_PARAM_DECL, ODIN3_AST_F_PARAM_DECL_DT_SHIFT, ODIN3_AST_F_PARAM_DECL_DT_MASK,
     ODIN3_AST_DT_TIME},
    {ODIN3_AST_FUNCTION_DECL, ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT, ODIN3_AST_F_FUNCTION_DECL_DT_MASK,
     ODIN3_AST_DT_TIME},
};

/* STRENGTH's S0/S1 limits: drive values in both, a charge in S0 only. */
static bool strength_ok(const odin3_ast_node *rec) {
    uint32_t s0 = (rec->flags & ODIN3_AST_F_STRENGTH_S0_MASK) >> ODIN3_AST_F_STRENGTH_S0_SHIFT;
    uint32_t s1 = (rec->flags & ODIN3_AST_F_STRENGTH_S1_MASK) >> ODIN3_AST_F_STRENGTH_S1_SHIFT;
    if (rec->sub == ODIN3_AST_STRENGTH_CHARGE) {
        return s0 <= ODIN3_AST_CHARGE_LARGE && s1 == 0;
    }
    return s0 <= ODIN3_AST_DRIVE_SUPPLY && s1 <= ODIN3_AST_DRIVE_SUPPLY;
}

static bool fields_ok(const odin3_ast_node *rec) {
    for (size_t i = 0; i < sizeof FIELDS / sizeof FIELDS[0]; i++) {
        if (FIELDS[i].kind == rec->kind &&
            (uint32_t)(rec->flags & FIELDS[i].mask) >> FIELDS[i].shift > FIELDS[i].max) {
            return false;
        }
    }
    return rec->kind != ODIN3_AST_STRENGTH || strength_ok(rec);
}

static void rule1_scalars(odin3_ast_checker *cx, const odin3_ast_kind_info *info) {
    const odin3_ast_node *rec = cx->rec;
    if (rec->sub >= info->sub_count && !(info->sub_count == 0 && rec->sub == 0)) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "sub-kind out of range");
    }
    if ((rec->flags & ~info->flags) != 0) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "flag bit the kind does not define");
    } else if (!fields_ok(rec)) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "flag field value out of range");
    }
}

/* Payload exactly when the kind takes one; false when the record's span must not be read. */
static bool rule1_payload(odin3_ast_checker *cx, const odin3_ast_kind_info *info) {
    const odin3_ast_node *rec = cx->rec;
    if (info->payload == ODIN3_AST_PAYLOAD_NONE) {
        if (rec->nchild == 0 && rec->child != 0) {
            odin3_ast_check_node_fail(cx, RULE_KIND, "payload on a kind without one");
        }
        return true;
    }
    const odin3_ast_payload_rec *pay = odin3_ast_payload_rec_of(cx->ast, rec->child);
    if (pay == NULL || pay->kind != info->payload) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "missing, invalid or mistyped payload");
    }
    if (rec->nchild != 0) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "children on a payload kind");
    }
    return false;
}

/* FULL: locs decode (or are 0); loc ≤ end when both lie in one buffer. */
static void rule1_locs(odin3_ast_checker *cx) {
    const odin3_ast_node *rec = cx->rec;
    odin3_srcpos pos;
    if (rec->loc != 0 && !odin3_srcman_decode(cx->sm, (odin3_loc){rec->loc}, &pos)) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "loc does not decode");
        return;
    }
    odin3_srcbuf_id end_buf = odin3_srcman_buffer_of(cx->sm, (odin3_loc){rec->end});
    if (rec->end != 0 && end_buf.v == 0) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "end does not decode");
    } else if (rec->loc > rec->end && rec->end != 0 &&
               odin3_srcman_buffer_of(cx->sm, (odin3_loc){rec->loc}).v == end_buf.v) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "loc after end in one buffer");
    }
}

/* --- rule 2: the slot table ---------------------------------------------------------------- */

/* The flag that is set exactly when the kind's tail is empty (DEFAULT, STAR); 0 for none. */
static uint16_t empty_tail_flag(uint8_t kind) {
    if (kind == ODIN3_AST_CASE_ITEM) {
        return ODIN3_AST_F_CASE_ITEM_DEFAULT;
    }
    return kind == ODIN3_AST_EVENT_CONTROL ? ODIN3_AST_F_EVENT_CONTROL_STAR : 0;
}

/* The tail bounds, with CASE_ITEM's and EVENT_CONTROL's flag-dependent emptiness. */
static void rule2_count(odin3_ast_checker *cx, const odin3_ast_kind_info *info) {
    uint32_t count = cx->rec->nchild;
    if (count < info->nslots || (info->tail == ODIN3_AST_CLS_NONE && count != info->nslots)) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS, "child count differs from the slot count");
        return;
    }
    uint32_t tail = count - info->nslots;
    if (info->tail != ODIN3_AST_CLS_NONE && (tail < info->tail_min || tail > info->tail_max)) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS, "tail count out of bounds");
    }
    uint16_t empty_flag = empty_tail_flag(cx->rec->kind);
    if (empty_flag != 0 && ((cx->rec->flags & empty_flag) != 0) != (tail == 0)) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS,
                                  "tail must be empty exactly when the flag is set");
    }
}

/* HIER_PART: IDENT, or SELECT(bit) over IDENT. */
static bool hier_part_ok(const odin3_ast_checker *cx, odin3_ast_id part) {
    if (kind_at(cx, part) != ODIN3_AST_SELECT) {
        return kind_at(cx, part) == ODIN3_AST_IDENT;
    }
    return odin3_ast_sub(cx->ast, part) == ODIN3_AST_SELECT_BIT &&
           kind_at(cx, odin3_ast_check_kid(cx, part, 0)) == ODIN3_AST_IDENT;
}

/* One child at position idx (a valid ID below the parent): its class. */
static void rule2_class(odin3_ast_checker *cx, const odin3_ast_kind_info *info, uint32_t idx,
                        odin3_ast_id kid) {
    uint8_t cls =
        idx < info->nslots ? (uint8_t)(info->slots[idx] & ODIN3_AST_SLOT_CLASS) : info->tail;
    const odin3_ast_kind_info *kid_info = odin3_ast_kind_info_of(kind_at(cx, kid));
    bool fits =
        cls == ODIN3_AST_CLS_HIER_PART ? hier_part_ok(cx, kid) : odin3_ast_class_has(cls, kid_info);
    if (!fits) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS, "child of the wrong class for its slot");
    }
}

/* Zero entries, AST-5 order and classes of every child. */
static void rule2_children(odin3_ast_checker *cx, const odin3_ast_kind_info *info,
                           odin3_ast_span kids) {
    for (uint32_t i = 0; i < kids.n; i++) {
        odin3_ast_id kid = kids.ids[i];
        if (kid.v == 0 && i < info->nslots) {
            if ((info->slots[i] & ODIN3_AST_SLOT_OPT) == 0) {
                odin3_ast_check_node_fail(cx, RULE_SLOTS, "mandatory slot is 0");
            }
        } else if (kid.v == 0) {
            if ((info->tail_zero & cx->rec->flags) == 0) {
                odin3_ast_check_node_fail(cx, RULE_SLOTS, "0 tail entry outside E*0");
            }
        } else if (kid.v >= cx->id.v) {
            odin3_ast_check_node_fail(cx, RULE_SLOTS, "child not below its parent (AST-5)");
        } else {
            rule2_class(cx, info, i, kid);
        }
    }
}

/* What a LIST in slot `slot` of the current node may hold (§4.3's slot notes). */
typedef enum list_want {
    WANT_ANY,
    WANT_PARAMS,
    WANT_PORTS,
    WANT_PORT_DECLS,
    WANT_CONNS,
    WANT_DECLS
} list_want;

/* Per kind and slot: what a LIST there holds (WANT_ANY = no constraint). */
static const uint8_t LIST_WANT[ODIN3_AST_KIND_COUNT][ODIN3_AST_MAX_SLOTS] = {
    [ODIN3_AST_MODULE] = {WANT_PARAMS, WANT_PORTS},
    [ODIN3_AST_INSTANTIATION] = {WANT_CONNS},
    [ODIN3_AST_FUNCTION_DECL] = {WANT_ANY, WANT_PORT_DECLS, WANT_DECLS},
    [ODIN3_AST_TASK_DECL] = {WANT_PORT_DECLS, WANT_DECLS},
    [ODIN3_AST_SEQ_BLOCK] = {WANT_DECLS},
    [ODIN3_AST_PAR_BLOCK] = {WANT_DECLS},
};

static bool list_entry_ok(list_want want, odin3_ast_kind kind) {
    switch (want) {
    case WANT_PARAMS:
        return kind == ODIN3_AST_PARAM_DECL;
    case WANT_PORTS:
        return kind == ODIN3_AST_PORT_DECL || kind == ODIN3_AST_PORT_REF;
    case WANT_PORT_DECLS:
        return kind == ODIN3_AST_PORT_DECL;
    case WANT_CONNS:
        return kind == ODIN3_AST_CONNECTION;
    case WANT_DECLS:
        return odin3_ast_class_has(ODIN3_AST_CLS_D, odin3_ast_kind_info_of(kind));
    default:
        return true;
    }
}

/* The LISTs in the current node's slots hold what the slot notes say. */
static void rule2_lists(odin3_ast_checker *cx, const odin3_ast_kind_info *info,
                        odin3_ast_span kids) {
    for (uint32_t i = 0; i < info->nslots && i < kids.n; i++) {
        list_want want = (list_want)LIST_WANT[cx->rec->kind][i];
        if (want == WANT_ANY || kind_at(cx, kids.ids[i]) != ODIN3_AST_LIST ||
            kids.ids[i].v >= cx->id.v) {
            continue;
        }
        odin3_ast_span entries = odin3_ast_check_span(cx, kids.ids[i]);
        for (uint32_t j = 0; j < entries.n; j++) {
            if (entries.ids[j].v != 0 && !list_entry_ok(want, kind_at(cx, entries.ids[j]))) {
                odin3_ast_check_node_fail(cx, RULE_SLOTS, "LIST entry of the wrong kind");
            }
        }
    }
}

/* CASE_ITEM bodies: S under CASE, I under GEN_CASE. */
static void rule2_case_bodies(odin3_ast_checker *cx, odin3_ast_span kids) {
    odin3_ast_class want = cx->rec->kind == ODIN3_AST_CASE ? ODIN3_AST_CLS_S : ODIN3_AST_CLS_I;
    for (uint32_t i = 1; i < kids.n; i++) {
        if (kind_at(cx, kids.ids[i]) != ODIN3_AST_CASE_ITEM || kids.ids[i].v >= cx->id.v) {
            continue;
        }
        odin3_ast_id body = odin3_ast_check_kid(cx, kids.ids[i], 0);
        if (body.v != 0 && !odin3_ast_class_has(want, odin3_ast_kind_info_of(kind_at(cx, body)))) {
            odin3_ast_check_node_fail(cx, RULE_SLOTS, "case item body of the wrong class");
        }
    }
}

static void rule2_context(odin3_ast_checker *cx, const odin3_ast_kind_info *info,
                          odin3_ast_span kids) {
    uint8_t kind = cx->rec->kind;
    if (kind == ODIN3_AST_CASE || kind == ODIN3_AST_GEN_CASE) {
        rule2_case_bodies(cx, kids);
    }
    if (kind == ODIN3_AST_GEN_BLOCK && kids.n > 0 && kids.ids[0].v != 0 &&
        odin3_ast_form_of(cx->ast) == ODIN3_AST_FORM_PARSED) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS, "GEN_BLOCK index in the parsed form");
    }
    if (kind == ODIN3_AST_SELECT && kids.n == info->nslots &&
        (cx->rec->sub == ODIN3_AST_SELECT_BIT) == (kids.ids[2].v != 0)) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS,
                                  "SELECT's third slot is present exactly for a part select");
    }
    rule2_lists(cx, info, kids);
}

/* --- rule 3 (references), 4 (names), 5 (heights) per node --------------------------------- */

static void rule3_mark(odin3_ast_checker *cx, odin3_ast_span kids) {
    for (uint32_t i = 0; i < kids.n; i++) {
        odin3_ast_id kid = kids.ids[i];
        if (kid.v == 0 || kid.v >= cx->end) {
            continue;
        }
        odin3_ast_check_site site = {RULE_REFS, label_of(odin3_ast_rec(cx->ast, kid)), kid.v};
        odin3_ast_kind kind = kind_at(cx, kid);
        if (kind == ODIN3_AST_ATTR) {
            odin3_ast_check_fail(cx, site, "ATTR in a child span");
        } else if (kind == ODIN3_AST_UNIT) {
            odin3_ast_check_fail(cx, site, "UNIT in a child span");
        } else if (cx->refs[kid.v] != 0) {
            odin3_ast_check_fail(cx, site, "node in more than one span");
        }
        cx->refs[kid.v] = REF_SPAN;
    }
}

/* True when some UDP_DECL in the store is called name (an unnamed UDP instance is legal). */
static bool udp_declared(const odin3_ast_checker *cx, uint32_t name) {
    for (uint32_t i = 1; i < cx->end; i++) {
        const odin3_ast_node *rec = odin3_ast_rec(cx->ast, (odin3_ast_id){i});
        if (rec->kind == ODIN3_AST_UDP_DECL && rec->name == name) {
            return true;
        }
    }
    return false;
}

static void rule4_names(odin3_ast_checker *cx, const odin3_ast_kind_info *info,
                        odin3_ast_span kids) {
    uint32_t name = cx->rec->name;
    if (info->name_rule == ODIN3_AST_NAME_NO && name != 0) {
        odin3_ast_check_node_fail(cx, RULE_NAME, "name on a kind without one");
    } else if (info->name_rule == ODIN3_AST_NAME_REQ && name == 0) {
        odin3_ast_check_node_fail(cx, RULE_NAME, "missing name");
    }
    if (name >= odin3_strtab_count(odin3_design_strtab(odin3_ast_design(cx->ast)))) {
        odin3_ast_check_node_fail(cx, RULE_NAME, "name is not a strtab ID");
    }
    if (cx->rec->kind != ODIN3_AST_INSTANTIATION) {
        return;
    }
    for (uint32_t i = info->nslots; i < kids.n; i++) {
        if (kind_at(cx, kids.ids[i]) == ODIN3_AST_INSTANCE &&
            odin3_ast_name(cx->ast, kids.ids[i]) == 0 && !udp_declared(cx, name)) {
            odin3_ast_check_node_fail(cx, RULE_NAME, "INSTANCE under INSTANTIATION without a name");
        }
    }
}

static void rule5_height(odin3_ast_checker *cx, odin3_ast_span kids) {
    uint32_t height = 0;
    for (uint32_t i = 0; i < kids.n; i++) {
        uint32_t kid = kids.ids[i].v;
        if (kid != 0 && kid < cx->id.v && cx->heights[kid] > height) {
            height = cx->heights[kid];
        }
    }
    cx->heights[cx->id.v] = height + 1;
}

/* --- rule 6 per node: the NUMBER payload ---------------------------------------------------- */

static const char *number_problem(const odin3_ast_number *num) {
    if (num == NULL || num->bits == NULL || num->nbits == 0) {
        return "NUMBER payload without bits";
    }
    if (num->nbits > ODIN3_AST_MAX_NUMBER_BITS || num->width > ODIN3_AST_MAX_NUMBER_BITS) {
        return "NUMBER payload wider than the cap";
    }
    bool any_xz = false;
    bool any_z = false;
    for (uint32_t i = 0; i < num->nbits; i++) {
        if (num->bits[i] > ODIN3_BIT_Z) {
            return "NUMBER bit not 0 1 x z";
        }
        any_xz = any_xz || num->bits[i] >= ODIN3_BIT_X;
        any_z = any_z || num->bits[i] == ODIN3_BIT_Z;
    }
    if (any_xz != num->has_xz) {
        return "NUMBER has_xz disagrees with the bits";
    }
    return num->has_question && !any_z ? "NUMBER has_question without a z bit" : NULL;
}

static void rule6_number(odin3_ast_checker *cx) {
    const odin3_ast_payload_rec *pay = odin3_ast_payload_rec_of(cx->ast, cx->rec->child);
    if (pay == NULL || pay->kind != ODIN3_AST_PAYLOAD_NUMBER) {
        return; /* rule 1 reported it */
    }
    const char *why = number_problem(pay->u.number);
    if (why != NULL) {
        odin3_ast_check_node_fail(cx, RULE_TABLES, why);
    }
}

/* --- the per-node sweep -------------------------------------------------------------------- */

/* Rules 2–5 and 7 over a readable span. */
static void check_span(odin3_ast_checker *cx, const odin3_ast_kind_info *info) {
    if (!span_in_table(cx->ast, cx->rec)) {
        odin3_ast_check_node_fail(cx, RULE_SLOTS, "span outside the child table");
        cx->heights[cx->id.v] = 1;
        rule4_names(cx, info, (odin3_ast_span){NULL, 0});
        return;
    }
    odin3_ast_span kids = odin3_ast_check_span(cx, cx->id);
    rule2_count(cx, info);
    rule2_children(cx, info, kids);
    rule2_context(cx, info, kids);
    if (cx->full) {
        rule3_mark(cx, kids);
    }
    rule4_names(cx, info, kids);
    rule5_height(cx, kids);
    if (cx->full && odin3_ast_form_of(cx->ast) == ODIN3_AST_FORM_ELABORATED) {
        odin3_ast_check_elab_node(cx);
    }
}

static void check_node(odin3_ast_checker *cx, uint32_t id) {
    cx->id = (odin3_ast_id){id};
    cx->rec = odin3_ast_rec(cx->ast, cx->id);
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(cx->rec->kind);
    if (info == NULL) {
        odin3_ast_check_node_fail(cx, RULE_KIND, "unknown kind");
        cx->heights[id] = 1;
        return;
    }
    rule1_scalars(cx, info);
    if (cx->full) {
        rule1_locs(cx);
    }
    if (rule1_payload(cx, info)) {
        check_span(cx, info);
        return;
    }
    cx->heights[id] = 1;
    rule4_names(cx, info, (odin3_ast_span){NULL, 0});
    if (cx->full && info->payload == ODIN3_AST_PAYLOAD_NUMBER) {
        rule6_number(cx);
    }
    if (cx->full && odin3_ast_form_of(cx->ast) == ODIN3_AST_FORM_ELABORATED) {
        odin3_ast_check_elab_node(cx);
    }
}

/* --- store-level parts ---------------------------------------------------------------------- */

/* Rule 6's attribute span of one map entry, and rule 3's single reference of its ATTRs. */
static void check_attr_entry(odin3_ast_checker *cx, odin3_kv entry) {
    uint32_t start = (uint32_t)entry.value;
    uint32_t count = (uint32_t)(entry.value >> ATTR_COUNT_SHIFT);
    const odin3_ast_node *rec =
        entry.key < cx->end ? odin3_ast_rec(cx->ast, (odin3_ast_id){(uint32_t)entry.key}) : NULL;
    odin3_ast_check_site site = {RULE_TABLES, "attrs", (uint32_t)entry.key};
    if (rec == NULL || rec->kind == ODIN3_AST_ATTR) {
        odin3_ast_check_fail(cx, site, "attributes on a missing node or an ATTR");
    }
    if (count == 0 || (uint64_t)start + count > cx->ast->children.len) {
        odin3_ast_check_fail(cx, site, "attribute span outside the child table");
        return;
    }
    const odin3_ast_id *ids = (const odin3_ast_id *)cx->ast->children.data + start;
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i].v >= cx->end || kind_at(cx, ids[i]) != ODIN3_AST_ATTR) {
            odin3_ast_check_fail(cx, site, "attribute entry is not an ATTR");
            continue;
        }
        if (cx->refs[ids[i].v] != 0) {
            odin3_ast_check_site twice = {RULE_REFS, "ATTR", ids[i].v};
            odin3_ast_check_fail(cx, twice, "ATTR attached twice or also in a child span");
        }
        cx->refs[ids[i].v] = REF_ATTR;
    }
}

/* Rules 3 and 6 over the attribute map, then rule 3's unreachable count (informational). */
static void check_refs(odin3_ast_checker *cx) {
    size_t cursor = 0;
    odin3_kv entry;
    while (cx->ast->attrs != NULL && odin3_u64map_next(cx->ast->attrs, &cursor, &entry)) {
        check_attr_entry(cx, entry);
    }
    uint32_t unreachable = 0;
    for (uint32_t id = 1; id < cx->end; id++) {
        unreachable += cx->refs[id] == 0 && kind_at(cx, (odin3_ast_id){id}) != ODIN3_AST_UNIT;
    }
    if (unreachable > 0) {
        odin3_log(ODIN3_LOG_INFO, "ast check: rule 3: %" PRIu32 " unreachable non-root nodes",
                  unreachable);
    }
}

static void check_heights(odin3_ast_checker *cx) {
    uint32_t max = 0;
    for (uint32_t id = 1; id < cx->end; id++) {
        max = cx->heights[id] > max ? cx->heights[id] : max;
    }
    odin3_ast_check_site site = {RULE_HEIGHT, "store", 0};
    if (max != odin3_ast_max_height(cx->ast)) {
        odin3_ast_check_fail(cx, site, "recorded maximum height is wrong");
    }
    if (max > ODIN3_AST_MAX_DEPTH) {
        odin3_ast_check_fail(cx, site, "height above ODIN3_AST_MAX_DEPTH");
    }
}

static void check_comments(odin3_ast_checker *cx) {
    const odin3_ast_comment *items = cx->ast->comments.data;
    for (uint32_t i = 1; i < cx->ast->comments.len; i++) {
        if (items[i - 1].loc.v > items[i].loc.v) {
            odin3_ast_check_site site = {RULE_TABLES, "comment", i};
            odin3_ast_check_fail(cx, site, "comments not sorted by loc");
        }
    }
}

static void check_store(odin3_ast_checker *cx) {
    check_heights(cx);
    if (!cx->full) {
        return;
    }
    check_refs(cx);
    if (cx->ast->finished) {
        check_comments(cx);
    }
    if (odin3_ast_form_of(cx->ast) == ODIN3_AST_FORM_ELABORATED) {
        odin3_ast_check_elab_store(cx);
    }
    const odin3_symtab *st = odin3_ast_symtab(cx->ast);
    if (st != NULL && odin3_symtab_check(st, cx->ast) != ODIN3_OK) {
        cx->errors[RULE_SYMTAB]++; /* odin3_symtab_check logged each violation */
    }
}

/* --- entry point ---------------------------------------------------------------------------- */

static odin3_status checker_init(odin3_ast_checker *cx) {
    cx->heights = odin3_util_calloc((size_t)cx->end * sizeof *cx->heights);
    if (cx->heights == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (!cx->full) {
        return ODIN3_OK;
    }
    cx->refs = odin3_util_calloc(cx->end);
    odin3_srcman *sm = NULL;
    if (cx->refs == NULL || odin3_design_get_srcman(cx->ast->design, &sm) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    cx->sm = sm;
    return ODIN3_OK;
}

static uint32_t error_total(const odin3_ast_checker *cx) {
    uint32_t total = 0;
    for (uint32_t rule = 1; rule <= ODIN3_AST_CHECK_RULES; rule++) {
        if (cx->errors[rule] > SHOW_PER_RULE) {
            odin3_log(ODIN3_LOG_ERROR,
                      "ast check: rule %" PRIu32 ": %" PRIu32 " more errors not shown", rule,
                      cx->errors[rule] - SHOW_PER_RULE);
        }
        total += cx->errors[rule];
    }
    return total;
}

odin3_status odin3_ast_check(const odin3_ast *ast, odin3_check_level level) {
    if (ast == NULL || (level != ODIN3_CHECK_FAST && level != ODIN3_CHECK_FULL)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_check: NULL store or unknown level");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast_checker cx = {
        .ast = ast, .full = level == ODIN3_CHECK_FULL, .end = odin3_ast_node_end(ast).v};
    odin3_status status = checker_init(&cx);
    if (status == ODIN3_OK) {
        for (uint32_t id = 1; id < cx.end; id++) {
            check_node(&cx, id);
        }
        check_store(&cx);
        status = error_total(&cx) == 0 ? ODIN3_OK : ODIN3_ERR_CHECK;
    }
    odin3_util_free(cx.heights);
    odin3_util_free(cx.refs);
    return status;
}
