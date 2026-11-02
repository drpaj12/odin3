/*
 * elab1_write.c — pass W (2D spec §3.4, EL-4): one specialization's MODULE written through the 2A
 * builders, bottom up, replaying pass E's decision log. Task 11 writes the header parameters as
 * PARAM_DECL[localparam]{IN_HEADER} with their final values (point (bb)'s spellings), the ANSI
 * ports one PORT_DECL per declarator, NET_DECL and VAR_DECL with NUMBER bounds, CONT_ASSIGN, the
 * body's parameters and localparams, and each INSTANTIATION named after its specialization with
 * one named CONNECTION per parameter and per port; expressions as written, but with the select
 * bounds and widths and the replication counts folded to NUMBERs (the elaborated form's resolved
 * constants). Task 12 adds ordered connections (resolved by elab1_inst.c), instance and gate
 * arrays expanded to their elements with sliced connections, cell-type and black-box instances
 * with their parameter records, and GATE_DECL. Task 14 adds the generate blocks: the module's
 * items are walked with a worklist, each selected block (replayed from the decision log, pass E's
 * scopes taken in the same order) written as a GEN_BLOCK with its name and loop index, a genvar
 * read written as its NUMBER and a HIER_NAME into the generate blocks resolved (G7) with its
 * indices folded. A kind not landed yet is reported (point (l));
 * after any error nothing is built, though the walk goes on to report.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "frontends/elab/netlist_locs.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    E1_SLOT_PARAMS = 0,
    E1_SLOT_PORTS = 1,
    E1_ITEMS = 2,
    E1_INT_BITS = 32,
    E1_NET_RANGE_SLOT = 1,
    E1_NET_DELAY_SLOT = 2,
    E1_INT64_BITS = 64,
    W_UNNAMED_TEXT = 40 /* "$unnamed_block$" and a 32-bit count */
};

static const uint32_t E1_NONE = UINT32_MAX;

/* The writer of one specialization. */
typedef struct e1w {
    odin3_elab1 *e1;
    odin3_elab1_env *env;
    const odin3_ast *ast; /* parsed */
    odin3_ast *out;       /* elaborated */
    const odin3_elab1_spec *spec;
    bool building;
    odin3_status st;
    uint32_t cursor; /* the next decision-log entry */
    uint32_t used;
    bool replay_failed;
    bool dropped;
    uint32_t scope;            /* the scope being written (pass E's) */
    uint32_t next_scope;       /* the next scope a selected block takes (pass E's creation order) */
    odin3_vec walk;            /* e1w_frame */
    uint32_t unnamed;          /* the source unnamed blocks with declarations numbered so far */
    odin3_u64map *unnamed_ids; /* source block node -> its k (generate iterations share it) */
    odin3_u64map *locals;      /* names local to the enclosing blocks and subroutines -> depth */
    bool in_initial;           /* the statement being copied is an INITIAL's */
    odin3_ast_id next_item;    /* the item after the one being written in its body (0: none) */
} e1w;

static bool live(const e1w *wr) {
    return wr->building && wr->st == ODIN3_OK && !wr->e1->stopped;
}

static void halt(e1w *wr) {
    wr->building = false;
}

static void fail(e1w *wr, odin3_status st) {
    if (st != ODIN3_OK && wr->st == ODIN3_OK) {
        wr->st = st;
    }
    wr->building = false;
}

/* A failed step: a diagnostic was reported (PARSE: stop building) or worse (the status). */
static void stop_on(e1w *wr, odin3_status st) {
    if (st == ODIN3_ERR_PARSE) {
        halt(wr);
    } else {
        fail(wr, st);
    }
}

static odin3_ast_spec spec_of(const e1w *wr, uint32_t kind, odin3_ast_id from) {
    return (odin3_ast_spec){.kind = (uint8_t)kind,
                            .loc = odin3_ast_loc(wr->ast, from).v,
                            .end = odin3_ast_end(wr->ast, from).v};
}

static uint32_t w_mark(const e1w *wr) {
    return live(wr) ? odin3_ast_mark(wr->out) : 0;
}

static void w_push(e1w *wr, odin3_ast_id id) {
    if (live(wr)) {
        odin3_status st = odin3_ast_push(wr->out, id);
        if (st != ODIN3_OK) {
            fail(wr, st);
        }
    }
}

/* A body item pushed unless it came to nothing (every declarator merged into a port). */
static void w_push_item(e1w *wr, odin3_ast_id id) {
    if (id.v != 0) {
        w_push(wr, id);
    }
}

bool odin3_elab1_node_room(odin3_elab1 *e1, odin3_ast_id from) {
    uint32_t made = odin3_ast_node_end(e1->elab).v - 1;
    if (made < e1->limits->max_elab_nodes) {
        return true;
    }
    odin3_elab1_fatal(e1, odin3_ast_loc(e1->parsed, from),
                      "elaboration needs more than %" PRIu32 " elaborated nodes",
                      e1->limits->max_elab_nodes);
    return false;
}

/* The elaborated-node cap before a make (point (aa)): fatal at the node being written. */
static bool w_room(e1w *wr, odin3_ast_id from) {
    if (odin3_elab1_node_room(wr->e1, from)) {
        return true;
    }
    halt(wr);
    return false;
}

typedef struct w_attrs_of {
    odin3_ast_id from;  /* the parsed node whose attributes are written */
    odin3_ast_id made;  /* the elaborated node they are attached to */
    odin3_ast_id then;  /* a second parsed node merged in after from's (0: none); it wins a name */
    uint8_t unit_flags; /* the MODULE of a --lib unit: its ODIN3_UNIT_* become attributes (EL-4) */
    odin3_bytes loc_text; /* -netlist: the `// LOC` comment of a CONT_ASSIGN, as an internal ATTR */
} w_attrs_of;

static void w_attach_attrs(e1w *wr, const w_attrs_of *at);

/* A node over the pending children from mark, without from's attributes. */
static odin3_ast_id w_make_plain(e1w *wr, const odin3_ast_spec *spec, uint32_t mark,
                                 odin3_ast_id from) {
    odin3_ast_id id = {0};
    if (!live(wr) || !w_room(wr, from)) {
        return id;
    }
    odin3_status st = odin3_ast_make_marked(wr->out, spec, mark, &id);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
    return id;
}

/* w_make_plain, then the attributes of from attached (EL-4: evaluated to NUMBER or STRING). */
static odin3_ast_id w_make_marked(e1w *wr, const odin3_ast_spec *spec, uint32_t mark,
                                  odin3_ast_id from) {
    odin3_ast_id id = w_make_plain(wr, spec, mark, from);
    w_attach_attrs(wr, &(w_attrs_of){.from = from, .made = id});
    return id;
}

/* --- constants (point (bb)) ----------------------------------------------------------------- */

/* A NUMBER payload made from num, at from's location. */
static odin3_ast_id w_number(e1w *wr, const odin3_ast_number *num, odin3_ast_spec *spec,
                             odin3_ast_id from) {
    odin3_status st =
        odin3_ast_number_new(wr->out, odin3_ast_loc(wr->ast, from), num, &spec->payload);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    return w_make_marked(wr, spec, odin3_ast_mark(wr->out), from);
}

/* The bytes of a STRING value that a STRING node can hold: whole known bytes, none of them NUL
 * (a NUL byte cannot be a name; the empty string is EMPTY). A string that was widened (`n == 0 ?
 * "MULT" : "FMA"` is 32 bits, its leading byte NUL) is a number: the same bits. */
static bool string_leaf_bytes(odin3_arena *arena, const odin3_cval *val, odin3_bytes *bytes) {
    *bytes = (odin3_bytes){NULL, 0};
    if (val == NULL || (val->flags & ODIN3_CVAL_STRING) == 0 || val->width % CHAR_BIT != 0 ||
        !odin3_cval_is_known(val)) {
        return false;
    }
    if (odin3_cval_string_bytes(arena, val, bytes) != ODIN3_OK) {
        return false;
    }
    return bytes->len == 0 || memchr(bytes->ptr, '\0', bytes->len) == NULL;
}

/* A STRING value as a STRING node's spec (the empty string: EMPTY, no name). */
static odin3_status string_spec(odin3_elab1 *e1, odin3_bytes bytes, odin3_ast_spec *spec) {
    spec->kind = ODIN3_AST_STRING;
    if (bytes.len == 0) {
        spec->flags = ODIN3_AST_F_STRING_EMPTY;
        return ODIN3_OK;
    }
    return odin3_ast_intern(e1->elab, bytes, &spec->name);
}

/* A NUMBER's spec and payload for a value (point (bb)), or num in the integer form. */
static odin3_status number_spec(odin3_elab1 *e1, odin3_arena *arena, const odin3_elab1_leaf *leaf,
                                odin3_ast_spec *spec) {
    odin3_cval_int num = {leaf->num, E1_INT_BITS, true};
    odin3_cval made = {0};
    const odin3_cval *val = leaf->val;
    odin3_status st = val == NULL ? odin3_cval_from_i64(arena, &num, &made) : ODIN3_OK;
    val = val == NULL ? &made : val;
    odin3_ast_number payload;
    st = st != ODIN3_OK ? st : odin3_cval_to_number(arena, val, &payload);
    st = st != ODIN3_OK
             ? st
             : odin3_ast_number_new(e1->elab, (odin3_loc){spec->loc}, &payload, &spec->payload);
    bool unsized = leaf->val != NULL && (val->flags & ODIN3_CVAL_UNSIZED) != 0;
    spec->kind = ODIN3_AST_NUMBER;
    spec->sub = unsized ? leaf->base : (uint8_t)ODIN3_AST_NUMBER_BIN;
    spec->sub = leaf->val == NULL ? (uint8_t)ODIN3_AST_NUMBER_DEC : spec->sub;
    spec->flags = (val->flags & ODIN3_CVAL_SIGNED) != 0 ? ODIN3_AST_F_NUMBER_SIGNED : 0;
    return st;
}

odin3_status odin3_elab1_make_leaf(odin3_elab1 *e1, odin3_arena *arena,
                                   const odin3_elab1_leaf *leaf, odin3_ast_id *out) {
    *out = (odin3_ast_id){0};
    if (!odin3_elab1_node_room(e1, leaf->from)) {
        return ODIN3_ERR_PARSE;
    }
    odin3_ast_spec spec = {.loc = odin3_ast_loc(e1->parsed, leaf->from).v,
                           .end = odin3_ast_end(e1->parsed, leaf->from).v};
    odin3_bytes bytes;
    bool string = string_leaf_bytes(arena, leaf->val, &bytes);
    odin3_status st = string ? string_spec(e1, bytes, &spec) : number_spec(e1, arena, leaf, &spec);
    return st != ODIN3_OK ? st : odin3_ast_make(e1->elab, &spec, NULL, 0, out);
}

/* A leaf through odin3_elab1_make_leaf while building. */
static odin3_ast_id w_leaf(e1w *wr, const odin3_elab1_leaf *leaf) {
    odin3_ast_id id = {0};
    if (!live(wr)) {
        return id;
    }
    odin3_status st = odin3_elab1_make_leaf(wr->e1, wr->env->arena, leaf, &id);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
    }
    return id;
}

/* A known 32-bit value as a NUMBER: sub DEC, SIGNED, payload width 32 (the integer form). */
static odin3_ast_id w_int(e1w *wr, int64_t value, odin3_ast_id from) {
    odin3_elab1_leaf leaf = {NULL, 0, value, from};
    return w_leaf(wr, &leaf);
}

/* A final value (a parameter's, a CONNECTION's): a STRING, or a NUMBER whose sub is the source
 * literal's base while unsized (payload width 0) and BIN otherwise, SIGNED per value. */
static odin3_ast_id w_value(e1w *wr, const odin3_cval *val, uint8_t base, odin3_ast_id from) {
    odin3_elab1_leaf leaf = {val, base, 0, from};
    return w_leaf(wr, &leaf);
}

/* An ATTR's value evaluated to a NUMBER or STRING (EL-4); absent when the source has none. */
static odin3_ast_id w_attr_value(e1w *wr, odin3_ast_id value) {
    if (value.v == 0) {
        return value;
    }
    odin3_cval val = {0};
    uint8_t base = ODIN3_AST_NUMBER_DEC;
    odin3_elab1_eval_req req = {value, "attribute value", &base};
    odin3_status st = odin3_elab1_eval(wr->env, &req, &val);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return (odin3_ast_id){0};
    }
    return w_value(wr, &val, base, value);
}

/* One ATTR copied from attr with its value evaluated; 0 when the writer stopped. */
static odin3_ast_id w_attr_one(e1w *wr, odin3_ast_id attr) {
    odin3_ast_id value = w_attr_value(wr, odin3_ast_child(wr->ast, attr, 0));
    odin3_ast_id id = {0};
    if (!live(wr) || !w_room(wr, attr)) {
        return id;
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_ATTR, attr);
    spec.name = odin3_ast_name(wr->ast, attr);
    spec.flags = odin3_ast_flags(wr->ast, attr);
    odin3_status st = odin3_ast_make(wr->out, &spec, &value, 1, &id);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
    return id;
}

/* Whether the attributes of node include one named name. */
static bool w_has_attr(const e1w *wr, odin3_ast_id node, uint32_t name) {
    odin3_ast_span attrs = odin3_ast_attrs(wr->ast, node);
    for (uint32_t i = 0; i < attrs.n; i++) {
        if (odin3_ast_name(wr->ast, attrs.ids[i]) == name) {
            return true;
        }
    }
    return false;
}

/* The attributes a --lib unit's MODULE carries beside its own (EL-4, spec 3.1a): the marker says
 * "elaborate under my flags, not the run's"; the others are those flags. */
static const struct {
    const char *name;
    unsigned bit;
} k_unit_attrs[] = {{"odin3_lib", ODIN3_UNIT_LIB},
                    {"odin3_lib_nolatches", ODIN3_UNIT_NOLATCHES},
                    {"odin3_lib_nomem2reg", ODIN3_UNIT_NOMEM2REG},
                    {"odin3_lib_vtr_compat", ODIN3_UNIT_VTR_COMPAT}};
enum { W_UNIT_ATTRS = 4, W_ATTR_MSG = 160 };

/* The internal ATTR that hands a CONT_ASSIGN's `// LOC` comment text to stage 2 (5.4c, D-3b): the
 * parsed store (with its comment table) is dropped before stage 2 runs. */
static uint32_t w_loc_attr(e1w *wr, const w_attrs_of *at, odin3_ast_id *ids) {
    if (at->loc_text.len == 0 || !live(wr)) {
        return 0;
    }
    odin3_ast_spec text = {.loc = odin3_ast_loc(wr->out, at->made).v,
                           .end = odin3_ast_end(wr->out, at->made).v};
    odin3_ast_spec spec = text;
    spec.kind = ODIN3_AST_ATTR;
    odin3_ast_id value = {0};
    odin3_status st = string_spec(wr->e1, at->loc_text, &text);
    st = st != ODIN3_OK ? st : odin3_ast_make(wr->out, &text, NULL, 0, &value);
    st = st != ODIN3_OK
             ? st
             : odin3_ast_intern(wr->out, odin3_bytes_cstr(ODIN3_NETLIST_ASSIGN_KEY), &spec.name);
    st = st != ODIN3_OK ? st : odin3_ast_make(wr->out, &spec, &value, 1, ids);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return 0;
    }
    return 1;
}

/* A value-less ATTR named text on made. */
static odin3_ast_id w_flag_attr(e1w *wr, odin3_ast_id made, const char *text) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_ATTR,
                           .loc = odin3_ast_loc(wr->out, made).v,
                           .end = odin3_ast_end(wr->out, made).v};
    const odin3_ast_id none = {0};
    odin3_ast_id attr = {0};
    odin3_status st = odin3_ast_intern(wr->out, odin3_bytes_cstr(text), &spec.name);
    st = st != ODIN3_OK ? st : odin3_ast_make(wr->out, &spec, &none, 1, &attr);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
    return attr;
}

/* The unit-flag attributes of at into ids[0 ..); how many. */
static uint32_t w_unit_attrs(e1w *wr, const w_attrs_of *at, odin3_ast_id *ids) {
    uint32_t count = 0;
    for (uint32_t i = 0; at->unit_flags != 0 && i < W_UNIT_ATTRS && live(wr); i++) {
        if ((at->unit_flags & k_unit_attrs[i].bit) != 0) {
            ids[count] = w_flag_attr(wr, at->made, k_unit_attrs[i].name);
            count += ids[count].v != 0 ? 1U : 0U;
        }
    }
    return count;
}

/* A user attribute named like the markers of a --lib unit (`odin3_lib*`) would pick the flags stage
 * 2 elaborates a module under (PR 2D-7b review M1): dropped, with a warning. */
static bool w_reserved_attr(e1w *wr, odin3_ast_id attr) {
    const char *name = odin3_elab1_str(wr->env->e1, odin3_ast_name(wr->ast, attr));
    static const char k_prefix[] = "odin3_lib";
    bool lib = strncmp(name, k_prefix, sizeof k_prefix - 1U) == 0;
    /* under -netlist the key of an assign's `// LOC` is stage 1's own: a user attribute of that
     * name would pick the location of a cell (5.4c review, agent default) */
    bool key = wr->e1->opts.netlist && strcmp(name, ODIN3_NETLIST_ASSIGN_KEY) == 0;
    if (!lib && !key) {
        return false;
    }
    char text[W_ATTR_MSG];
    (void)snprintf(text, sizeof text, "attribute '%s' is reserved for %s; ignored", name,
                   key ? "read_verilog -netlist" : "library reads");
    odin3_elab1_report(wr->env->e1, ODIN3_LOG_WARN, odin3_ast_loc(wr->ast, attr), text);
    return true;
}

/* The attributes of the parsed node from (then those of at->then, which win a name both give),
 * evaluated and attached to the elaborated node made, then the unit-flag attributes. */
/* The attributes of first and second (the later wins a name both give) evaluated into ids;
 * how many were made. */
static uint32_t w_copy_attrs(e1w *wr, const w_attrs_of *at, odin3_ast_span first,
                             odin3_ast_span second, odin3_ast_id *ids) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < first.n + second.n && live(wr); i++) {
        odin3_ast_id attr = i < first.n ? first.ids[i] : second.ids[i - first.n];
        if (w_reserved_attr(wr, attr)) {
            continue;
        }
        if (i < first.n && w_has_attr(wr, at->then, odin3_ast_name(wr->ast, attr))) {
            continue; /* the later declaration's value wins */
        }
        ids[count] = w_attr_one(wr, attr);
        count += ids[count].v != 0 ? 1U : 0U;
    }
    return count;
}

static void w_attach_attrs(e1w *wr, const w_attrs_of *at) {
    odin3_ast_span first = odin3_ast_attrs(wr->ast, at->from);
    odin3_ast_span second = odin3_ast_attrs(wr->ast, at->then);
    if ((first.n + second.n == 0 && at->unit_flags == 0 && at->loc_text.len == 0) ||
        at->made.v == 0 || !live(wr)) {
        return;
    }
    odin3_ast_id *ids = odin3_arena_alloc(
        wr->env->arena, ((size_t)first.n + second.n + W_UNIT_ATTRS + 1U) * sizeof *ids);
    if (ids == NULL) {
        fail(wr, ODIN3_ERR_NO_MEMORY);
        return;
    }
    uint32_t count = w_copy_attrs(wr, at, first, second, ids);
    count += w_unit_attrs(wr, at, ids + count);
    count += w_loc_attr(wr, at, ids + count);
    if (!live(wr) || count == 0) { /* every attribute was a dropped reserved odin3_lib* one */
        return;
    }
    odin3_status st = odin3_ast_attach(wr->out, at->made, ids, count);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
}

/* A declaration's RANGE with its folded bounds (pass E folded them). */
static odin3_ast_id w_range(e1w *wr, odin3_ast_id range) {
    if (range.v == 0) {
        return range;
    }
    int64_t msb = 0;
    int64_t lsb = 0;
    odin3_status st = odin3_elab1_env_range(wr->env, range, &msb, &lsb);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_int(wr, msb, odin3_ast_child(wr->ast, range, 0)));
    w_push(wr, w_int(wr, lsb, odin3_ast_child(wr->ast, range, 1)));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_RANGE, range);
    return w_make_marked(wr, &spec, mark, range);
}

/* --- expressions ---------------------------------------------------------------------------- */

/* The kinds the copier walks: expressions, statements and the process items. */
static bool copyable(odin3_ast_kind kind) {
    static const odin3_ast_kind k_kinds[] = {
        ODIN3_AST_HIER_NAME,
        ODIN3_AST_NUMBER,
        ODIN3_AST_STRING,
        ODIN3_AST_IDENT,
        ODIN3_AST_SELECT,
        ODIN3_AST_CONCAT,
        ODIN3_AST_REPLICATE,
        ODIN3_AST_UNARY,
        ODIN3_AST_BINARY,
        ODIN3_AST_TERNARY,
        ODIN3_AST_CALL,
        ODIN3_AST_ALWAYS,
        ODIN3_AST_INITIAL,
        ODIN3_AST_SEQ_BLOCK,
        ODIN3_AST_BLOCKING_ASSIGN,
        ODIN3_AST_NONBLOCKING_ASSIGN,
        ODIN3_AST_IF,
        ODIN3_AST_CASE,
        ODIN3_AST_CASE_ITEM,
        ODIN3_AST_FOR,
        ODIN3_AST_WHILE,
        ODIN3_AST_REPEAT,
        ODIN3_AST_TIMING_STMT,
        ODIN3_AST_TASK_CALL,
        ODIN3_AST_NULL_STMT,
        ODIN3_AST_EVENT_CONTROL,
        ODIN3_AST_EVENT_EXPR,
    };
    for (size_t i = 0; i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        if (k_kinds[i] == kind) {
            return true;
        }
    }
    return false;
}

/* Whether a CALL (system function) or TASK_CALL (system task) is one the elaborated form keeps
 * or folds; the rest are rejected (E20) or dropped (S3). */
static bool keeps_system(const e1w *wr, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, node);
    const char *name = odin3_elab1_str(wr->e1, odin3_ast_name(wr->ast, node));
    return (kind == ODIN3_AST_CALL || wr->in_initial) && odin3_elab1_system_kept(kind, name);
}

/* Whether node is walked: a kind not landed is reported (pending, or a 2D bug). */
static bool w_enter(e1w *wr, odin3_ast_id node) {
    if (odin3_elab1_pending(wr->e1, node)) {
        halt(wr);
        return false;
    }
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, node);
    if (!copyable(kind)) {
        odin3_elab1_unhandled(wr->e1, node);
        halt(wr);
        return false;
    }
    bool call = kind == ODIN3_AST_CALL || kind == ODIN3_AST_TASK_CALL;
    uint16_t flags = call ? odin3_ast_flags(wr->ast, node) : 0;
    if ((flags & ODIN3_AST_F_CALL_HIER) != 0 ||
        ((flags & ODIN3_AST_F_CALL_SYSTEM) != 0 && !keeps_system(wr, node))) {
        (void)odin3_elab1_reject(wr->e1, node); /* E21, E20: reported by the scan */
        halt(wr);
        return false;
    }
    return true;
}

/* The constant role of child idx of node when the elaborated form needs it folded (a part
 * select's bounds, a +:/-: width, a replication count); NULL when it is copied. */
static const char *fold_role(const odin3_ast *ast, odin3_ast_id node, uint32_t idx) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    if (kind == ODIN3_AST_REPLICATE) {
        return idx == 0 ? "replication count" : NULL;
    }
    if (kind == ODIN3_AST_REPEAT) {
        return idx == 0 ? "repeat count" : NULL;
    }
    if (kind != ODIN3_AST_SELECT) {
        return NULL;
    }
    switch (odin3_ast_sub(ast, node)) {
    case ODIN3_AST_SELECT_PART:
        return idx >= 1 ? "range bound" : NULL;
    case ODIN3_AST_SELECT_PART_PLUS:
    case ODIN3_AST_SELECT_PART_MINUS:
        return idx == 2 ? "range bound" : NULL;
    default:
        return NULL;
    }
}

/* A constant position folded to a NUMBER; alone: a replication outside a concatenation. */
static odin3_ast_id w_fold(e1w *wr, odin3_ast_id expr, const char *role, bool alone) {
    int32_t value = 0;
    odin3_status st = odin3_elab1_fold_int(wr->env, expr, role, &value);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return (odin3_ast_id){0};
    }
    bool replicate = strcmp(role, "replication count") == 0;
    if (value < 0 && replicate) {
        odin3_elab1_error(wr->e1, odin3_ast_loc(wr->ast, expr),
                          "replication count %" PRId32 " is negative", value);
        halt(wr);
        return (odin3_ast_id){0};
    }
    if (value == 0 && replicate && alone) {
        odin3_elab1_error(wr->e1, odin3_ast_loc(wr->ast, expr), "zero-width concatenation");
        halt(wr);
        return (odin3_ast_id){0};
    }
    if (value < 0 && strcmp(role, "repeat count") == 0) {
        value = 0; /* a negative repeat count runs zero times */
    }
    return w_int(wr, value, expr);
}

static odin3_ast_id w_copy_node(e1w *wr, odin3_ast_id node, uint32_t mark);

/* The undriven net of a reference into another module (Yosys: an auto-wire named by the dotted
 * path): its name interned, 0 when the reference is not one or on failure (wr->st set). */
static uint32_t w_foreign_name(e1w *wr, odin3_ast_id hier, uint32_t scope) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    bool foreign = false;
    uint32_t name = 0;
    odin3_status st = odin3_elab1_hier_foreign(wr->env, hier, scope, &foreign, &text);
    if (st == ODIN3_OK && foreign) {
        st = odin3_ast_intern(wr->out, odin3_bytes_cstr(text.data), &name);
    }
    odin3_strbuf_free(&text);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return 0;
    }
    return name;
}

/* A HIER_NAME into the module's generate blocks (G7), resolved; its indices NUMBERs. */
static odin3_ast_id w_hier(e1w *wr, odin3_ast_id hier) {
    uint32_t foreign = w_foreign_name(wr, hier, wr->scope);
    if (foreign != 0) { /* a reference into another module: the net w_hier_nets declared */
        odin3_ast_spec ispec = spec_of(wr, ODIN3_AST_IDENT, hier);
        ispec.name = foreign;
        return w_make_plain(wr, &ispec, w_mark(wr), hier);
    }
    odin3_status st = odin3_elab1_scope_ref(wr->e1, hier, wr->scope);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    odin3_ast_span parts = odin3_ast_children(wr->ast, hier);
    for (uint32_t i = 0; i < parts.n; i++) {
        odin3_ast_id part = parts.ids[i];
        if (odin3_ast_kind_of(wr->ast, part) != ODIN3_AST_SELECT) {
            w_push(wr, w_copy_node(wr, part, w_mark(wr)));
            continue;
        }
        uint32_t sel = w_mark(wr); /* SELECT(bit; IDENT, NUMBER, absent) */
        w_push(wr, w_copy_node(wr, odin3_ast_child(wr->ast, part, 0), w_mark(wr)));
        w_push(wr, w_fold(wr, odin3_ast_child(wr->ast, part, 1), "generate block index", false));
        w_push(wr, (odin3_ast_id){0});
        w_push(wr, w_copy_node(wr, part, sel));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_HIER_NAME, hier);
    return w_make_marked(wr, &spec, mark, hier);
}

/* The nodes written whole instead of copied: a HIER_NAME, and a genvar read inside a generate
 * loop (its iteration's value, a 32-bit signed NUMBER: EL-19). */
static bool w_special(e1w *wr, odin3_ast_id node, odin3_ast_id *out) {
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, node);
    if (kind == ODIN3_AST_HIER_NAME) {
        *out = w_hier(wr, node);
        return true;
    }
    if (kind == ODIN3_AST_CALL && (odin3_ast_flags(wr->ast, node) & ODIN3_AST_F_CALL_SYSTEM) != 0 &&
        strcmp(odin3_elab1_str(wr->e1, odin3_ast_name(wr->ast, node)), "$clog2") == 0) {
        *out = w_fold(wr, node, "$clog2 argument", false); /* EL-18: a NUMBER */
        return true;
    }
    if (kind != ODIN3_AST_IDENT) {
        return false;
    }
    uint32_t idx = odin3_elab1_env_lookup(wr->env, odin3_ast_name(wr->ast, node));
    const odin3_elab1_slot *slot = idx != E1_NONE ? odin3_elab1_env_slot(wr->env, idx) : NULL;
    int64_t value = 0;
    uint64_t depth = 0;
    if (odin3_u64map_get(wr->locals, odin3_ast_name(wr->ast, node), &depth) && depth > 0) {
        return false; /* a block, function or task local hides the genvar (EL-19) */
    }
    if (slot == NULL || slot->entry.kind != ODIN3_ENTRY_GENVAR || !slot->bound ||
        !odin3_cval_to_i64(&slot->entry.value, &value)) {
        return false;
    }
    *out = w_int(wr, value, node);
    return true;
}

/* The name of an unnamed block with declarations (EL-4, EL-23): `$unnamed_block$<k>`, k counting
 * per module in source order of the source block (every generate iteration shares its k), with
 * the SystemVerilog-extension warning. */
static uint32_t w_unnamed(e1w *wr, odin3_ast_id node) {
    char text[W_UNNAMED_TEXT];
    uint64_t k = 0;
    if (!odin3_u64map_get(wr->unnamed_ids, node.v, &k)) {
        k = ++wr->unnamed;
        odin3_status put = odin3_u64map_put(wr->unnamed_ids, (odin3_kv){node.v, k});
        if (put != ODIN3_OK) {
            fail(wr, put);
            return 0;
        }
    }
    int len = snprintf(text, sizeof text, "$unnamed_block$%" PRIu64, k);
    uint32_t name = 0;
    if (len <= 0 || (size_t)len >= sizeof text) {
        fail(wr, ODIN3_ERR_CHECK);
        return 0;
    }
    odin3_status st = odin3_ast_intern(wr->out, odin3_bytes_cstr(text), &name);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return 0;
    }
    odin3_elab1_warn_once(wr->e1, node,
                          "declaration in an unnamed block is a SystemVerilog extension; the "
                          "block is named '%s'",
                          text);
    return name;
}

static odin3_ast_id w_param_decl(e1w *wr, odin3_ast_id group, bool in_header);

/* A variable declared in a block, function or task: VAR_DECL[reg|integer], its declarators without
 * initialisers (not an expression walk: a block's declarations are written from inside one). */
static odin3_ast_id w_local_var(e1w *wr, odin3_ast_id group) {
    uint32_t sub = odin3_ast_sub(wr->ast, group);
    if (sub != ODIN3_AST_VAR_DECL_REG && sub != ODIN3_AST_VAR_DECL_INTEGER) {
        (void)odin3_elab1_reject(wr->e1, group); /* N17 */
        halt(wr);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
    odin3_ast_span kids = odin3_ast_children(wr->ast, group);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_VAR_DECL); i < kids.n; i++) {
        odin3_ast_id decl = kids.ids[i];
        if (odin3_ast_child(wr->ast, decl, 0).v != 0) {
            odin3_elab1_error(wr->e1, odin3_ast_loc(wr->ast, decl),
                              "an initialiser in a block or a subroutine is not supported");
            halt(wr);
        }
        uint32_t one = w_mark(wr);
        w_push(wr, (odin3_ast_id){0});
        for (uint32_t k = 1; k < odin3_ast_nchild(wr->ast, decl); k++) {
            w_push(wr, w_range(wr, odin3_ast_child(wr->ast, decl, k)));
        }
        odin3_ast_spec dspec = spec_of(wr, ODIN3_AST_DECLARATOR, decl);
        dspec.name = odin3_ast_name(wr->ast, decl);
        w_push(wr, w_make_marked(wr, &dspec, one, decl));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_VAR_DECL, group);
    spec.sub = (uint8_t)sub;
    spec.flags = odin3_ast_flags(wr->ast, group) & (uint16_t)ODIN3_AST_F_VAR_DECL_SIGNED;
    return w_make_marked(wr, &spec, mark, group);
}

/* A declaration of a block, function or task that is not a variable (Task 15's error). */
static void w_local_other(e1w *wr, odin3_ast_id group) {
    odin3_elab1_error(wr->e1, odin3_ast_loc(wr->ast, group),
                      "this declaration is not supported in a block or a subroutine");
    halt(wr);
}

/* The declarations of a block (a LIST of VAR_DECLs). */
static odin3_ast_id w_locals(e1w *wr, odin3_ast_id list) {
    uint32_t mark = w_mark(wr);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; i < entries.n; i++) {
        odin3_ast_id entry = entries.ids[i];
        if (odin3_elab1_pending(wr->e1, entry)) {
            halt(wr);
        } else if (odin3_ast_kind_of(wr->ast, entry) == ODIN3_AST_VAR_DECL) {
            w_push(wr, w_local_var(wr, entry));
        } else if (odin3_ast_kind_of(wr->ast, entry) == ODIN3_AST_PARAM_DECL) {
            w_push_item(wr, w_param_decl(wr, entry, false));
        } else {
            w_local_other(wr, entry);
        }
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list);
    return w_make_marked(wr, &spec, mark, list);
}

/* Enter or leave the names node (a SEQ_BLOCK, FUNCTION_DECL or TASK_DECL) declares. */
static void w_scope(e1w *wr, odin3_ast_id node, bool enter) {
    odin3_status st = odin3_elab1_scope_names(wr->locals, wr->ast, node, enter);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
}

/* The declarations LIST of a SEQ_BLOCK, FUNCTION_DECL or TASK_DECL. */
static odin3_ast_id locals_list(const e1w *wr, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, node);
    uint32_t idx = 2; /* a FUNCTION_DECL: range, ports, locals, body */
    if (kind == ODIN3_AST_SEQ_BLOCK) {
        idx = 0;
    } else if (kind == ODIN3_AST_TASK_DECL) {
        idx = 1;
    }
    return odin3_ast_child(wr->ast, node, idx);
}

/* Enter node's parameter scope (Task 15b, EL-4): when its declarations hold a PARAM_DECL, a scope
 * under the current one becomes the lookup scope and holds the parameters (evaluated lazily, in
 * order, where they are declared). Left by w_params_leave. */
static void w_params_enter(e1w *wr, odin3_ast_id node) {
    odin3_ast_id list = locals_list(wr, node);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    uint32_t scope = 0;
    bool any = false;
    for (uint32_t i = 0; list.v != 0 && i < entries.n; i++) {
        any = any || odin3_ast_kind_of(wr->ast, entries.ids[i]) == ODIN3_AST_PARAM_DECL;
    }
    if (!any) {
        return;
    }
    odin3_status st = odin3_elab1_scope_local(wr->env, wr->env->cur_scope, &scope);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return;
    }
    wr->env->cur_scope = scope;
    for (uint32_t i = 0; st == ODIN3_OK && i < entries.n; i++) {
        if (odin3_ast_kind_of(wr->ast, entries.ids[i]) == ODIN3_AST_PARAM_DECL) {
            st = odin3_elab1_env_add_params(wr->env, entries.ids[i]);
        }
    }
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
}

/* Leave the scope w_params_enter made for node (none when it declares no parameter). */
static void w_params_leave(e1w *wr, odin3_ast_id node) {
    const odin3_elab_scope *rec = odin3_elab1_scope_at(wr->env, wr->env->cur_scope);
    odin3_ast_id list = locals_list(wr, node);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; list.v != 0 && rec != NULL && i < entries.n; i++) {
        if (odin3_ast_kind_of(wr->ast, entries.ids[i]) == ODIN3_AST_PARAM_DECL) {
            wr->env->cur_scope = rec->parent;
            return;
        }
    }
}

/* Enter or leave a SEQ_BLOCK's names (when `at` holds: its first child, or its close). */
static void w_block_scope(e1w *wr, odin3_ast_id node, bool at, bool enter) {
    if (at && odin3_ast_kind_of(wr->ast, node) == ODIN3_AST_SEQ_BLOCK) {
        w_scope(wr, node, enter);
        if (enter) {
            w_params_enter(wr, node);
        } else {
            w_params_leave(wr, node);
        }
    }
}

/* A NULL_STMT standing for node (a statement the elaborated form drops, or one that was absent). */
static odin3_ast_id w_null_make(e1w *wr, odin3_ast_id node) {
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NULL_STMT, node);
    return w_make_marked(wr, &spec, w_mark(wr), node);
}

static void w_null_stmt(e1w *wr, odin3_ast_id node) {
    w_push(wr, w_null_make(wr, node));
}

/* Whether node is a system task other than the ones the elaborated form keeps (S3: removed). */
static bool dropped_task(const e1w *wr, odin3_ast_id node) {
    return node.v != 0 && odin3_ast_kind_of(wr->ast, node) == ODIN3_AST_TASK_CALL &&
           (odin3_ast_flags(wr->ast, node) & ODIN3_AST_F_TASK_CALL_SYSTEM) != 0 &&
           (odin3_ast_flags(wr->ast, node) & ODIN3_AST_F_TASK_CALL_HIER) == 0 &&
           !odin3_elab1_system_kept(ODIN3_AST_TASK_CALL,
                                    odin3_elab1_str(wr->e1, odin3_ast_name(wr->ast, node)));
}

/* The statement a TIMING_STMT outside an ALWAYS stands for (T11: its delay is dropped and the
 * statement kept); node itself when it is none. 0 when the timing control had no statement. */
static odin3_ast_id untimed(const e1w *wr, odin3_ast_id node) {
    while (node.v != 0 && odin3_ast_kind_of(wr->ast, node) == ODIN3_AST_TIMING_STMT) {
        node = odin3_ast_child(wr->ast, node, 1);
    }
    return node;
}

/* The child idx of node when it is more than a copy: a block's declarations, the control of an
 * assignment (dropped, T11), an absent statement of a TIMING_STMT (a NULL_STMT). True when the
 * child was written. */
static bool copy_special(e1w *wr, odin3_ast_id node, uint32_t idx, odin3_ast_id kid) {
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, node);
    bool assign = kind == ODIN3_AST_BLOCKING_ASSIGN || kind == ODIN3_AST_NONBLOCKING_ASSIGN;
    if (kind == ODIN3_AST_SEQ_BLOCK && idx == 0 && kid.v != 0) {
        w_push(wr, w_locals(wr, kid));
        return true;
    }
    if (kind == ODIN3_AST_TIMING_STMT && idx == 1 && kid.v == 0) {
        w_null_stmt(wr, node);
        return true;
    }
    if (assign && idx == 1 && kid.v != 0) {
        w_push(wr, (odin3_ast_id){0}); /* the intra-assignment delay: ignored (T11) */
        return true;
    }
    return false;
}

/* A copy of node (a leaf or an operator) whose children are the pending span from mark. */
static odin3_ast_id w_copy_node(e1w *wr, odin3_ast_id node, uint32_t mark) {
    if (!live(wr)) {
        return (odin3_ast_id){0};
    }
    odin3_ast_spec spec = spec_of(wr, odin3_ast_kind_of(wr->ast, node), node);
    spec.sub = (uint8_t)odin3_ast_sub(wr->ast, node);
    spec.flags = odin3_ast_flags(wr->ast, node);
    spec.name = odin3_ast_name(wr->ast, node);
    if (spec.kind == ODIN3_AST_NUMBER) {
        return w_number(wr, odin3_ast_number_of(wr->ast, node), &spec, node);
    }
    if (spec.kind == ODIN3_AST_SEQ_BLOCK && spec.name == 0 &&
        odin3_ast_nchild(wr->ast, odin3_ast_child(wr->ast, node, 0)) > 0) {
        spec.name = w_unnamed(wr, node);
    }
    return w_make_marked(wr, &spec, mark, node);
}

/* Child *kid of node as the elaborated form has it: a delay in front of a statement stripped
 * (T11), a system task removed (S3: omitted from a block, a NULL_STMT elsewhere). True when
 * the child was written here. */
static bool copy_dropped(e1w *wr, odin3_ast_id node, uint32_t idx, odin3_ast_id *kid) {
    odin3_ast_id orig = *kid;
    if (orig.v == 0 || odin3_ast_kind_of(wr->ast, node) == ODIN3_AST_ALWAYS) {
        return false;
    }
    odin3_ast_id inner = untimed(wr, orig);
    if (inner.v == 0) {
        w_null_stmt(wr, orig); /* `#5;` */
        return true;
    }
    if (dropped_task(wr, inner)) {
        bool in_block = odin3_ast_kind_of(wr->ast, node) == ODIN3_AST_SEQ_BLOCK && idx >= 1;
        if (!in_block) {
            w_null_stmt(wr, inner);
        }
        return true;
    }
    *kid = inner;
    return false;
}

static bool push_frame(e1w *wr, odin3_ast_id node) {
    odin3_elab1_frame *frame = odin3_vec_push(&wr->e1->frames);
    if (frame == NULL) {
        fail(wr, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    *frame = (odin3_elab1_frame){node.v, 0, w_mark(wr)};
    return true;
}

/* Whether the node being copied (the top frame) is an item of a concatenation. */
static bool in_concat(const e1w *wr, size_t base) {
    if (wr->e1->frames.len - 1 <= base) {
        return false;
    }
    const odin3_elab1_frame *up = odin3_vec_cat(&wr->e1->frames, wr->e1->frames.len - 2);
    return odin3_ast_kind_of(wr->ast, (odin3_ast_id){up->node}) == ODIN3_AST_CONCAT;
}

/* One step of the copier: the top frame's next child, or the node itself once they are made. */
static void copy_step(e1w *wr, size_t base, odin3_ast_id *result) {
    odin3_elab1_frame *top = odin3_vec_at(&wr->e1->frames, wr->e1->frames.len - 1);
    odin3_ast_id node = {top->node};
    odin3_ast_span kids = odin3_ast_children(wr->ast, node);
    if (top->next < kids.n) {
        uint32_t idx = top->next++;
        w_block_scope(wr, node, idx == 0, true);
        odin3_ast_id kid = kids.ids[idx];
        odin3_ast_id made = {0};
        if (copy_dropped(wr, node, idx, &kid)) {
            return;
        }
        const char *role = kid.v != 0 ? fold_role(wr->ast, node, idx) : NULL;
        if (copy_special(wr, node, idx, kid)) {
            return;
        }
        if (role != NULL) {
            bool alone = !in_concat(wr, base);
            w_push(wr, w_fold(wr, kid, role, alone));
        } else if (kid.v == 0 || !w_enter(wr, kid)) {
            w_push(wr, (odin3_ast_id){0});
        } else if (w_special(wr, kid, &made)) {
            w_push(wr, made);
        } else {
            (void)push_frame(wr, kid);
        }
        return;
    }
    uint32_t mark = top->mark;
    odin3_vec_pop(&wr->e1->frames);
    w_block_scope(wr, node, true, false);
    odin3_ast_id made = w_copy_node(wr, node, mark);
    if (wr->e1->frames.len > base) {
        w_push(wr, made);
    } else {
        *result = made;
    }
}

/* An expression as written (iterative, no recursion), with its resolved constants folded. */
static odin3_ast_id w_expr(e1w *wr, odin3_ast_id root) {
    odin3_ast_id result = {0};
    size_t base = wr->e1->frames.len;
    odin3_ast_id inner = untimed(wr, root);
    if (root.v != 0 && (inner.v == 0 || dropped_task(wr, inner))) {
        return w_null_make(wr, root); /* a task body that is a delay alone, or a system task */
    }
    root = inner;
    if (root.v == 0 || !w_enter(wr, root) || w_special(wr, root, &result) ||
        !push_frame(wr, root)) {
        return result;
    }
    while (wr->e1->frames.len > base && wr->st == ODIN3_OK) {
        copy_step(wr, base, &result);
    }
    while (wr->e1->frames.len > base) {
        odin3_vec_pop(&wr->e1->frames);
    }
    return result;
}

/* --- declarations --------------------------------------------------------------------------- */

/* A DECLARATOR to copy: its name and unpacked RANGEs are decl's, its value expression (E?) is
 * value's (the same declarator, or the merged object's for a port, C-1). */
typedef struct w_declarator_of {
    odin3_ast_id decl;
    odin3_ast_id value;
} w_declarator_of;

static odin3_ast_id w_declarator_valued(e1w *wr, const w_declarator_of *of) {
    uint32_t mark = w_mark(wr);
    odin3_ast_span kids = odin3_ast_children(wr->ast, of->decl);
    w_push(wr, w_expr(wr, odin3_ast_child(wr->ast, of->value, 0)));
    for (uint32_t i = 1; i < kids.n; i++) {
        w_push(wr, w_range(wr, kids.ids[i]));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_DECLARATOR, of->decl);
    spec.name = odin3_ast_name(wr->ast, of->decl);
    return w_make_marked(wr, &spec, mark, of->decl);
}

static odin3_ast_id w_declarator(e1w *wr, odin3_ast_id decl) {
    return w_declarator_valued(wr, &(w_declarator_of){decl, decl});
}

/* A parameter's DECLARATOR with its final value (pass E evaluated it). */
static odin3_ast_id w_param_declarator(e1w *wr, odin3_ast_id decl) {
    uint32_t idx = odin3_elab1_env_find(wr->env, wr->env->cur_scope, odin3_ast_name(wr->ast, decl));
    const odin3_elab1_slot *slot = idx != E1_NONE ? odin3_elab1_env_slot(wr->env, idx) : NULL;
    odin3_status st = slot != NULL ? odin3_elab1_env_value(wr->env, idx) : ODIN3_ERR_PARSE;
    if (st != ODIN3_OK) {
        stop_on(wr, st); /* a module parameter: reported in pass E; a local one, here */
        return (odin3_ast_id){0};
    }
    slot = odin3_elab1_env_slot(wr->env, idx);
    uint32_t mark = w_mark(wr);
    w_push(wr, w_value(wr, &slot->entry.value, slot->base, decl));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_DECLARATOR, decl);
    spec.name = odin3_ast_name(wr->ast, decl);
    return w_make_marked(wr, &spec, mark, decl);
}

/* The declarators of group after its slots, each through write. */
static void w_declarators(e1w *wr, odin3_ast_id group, odin3_ast_id (*write)(e1w *, odin3_ast_id)) {
    odin3_ast_span kids = odin3_ast_children(wr->ast, group);
    for (uint32_t i = odin3_ast_slot_count(odin3_ast_kind_of(wr->ast, group)); i < kids.n; i++) {
        w_push(wr, write(wr, kids.ids[i]));
    }
}

/* PARAM_DECL[localparam] with the final values (EL-4); RANGE, SIGNED and DT kept as declared. */
static odin3_ast_id w_param_decl(e1w *wr, odin3_ast_id group, bool in_header) {
    if (odin3_ast_sub(wr->ast, group) == ODIN3_AST_PARAM_DECL_SPECPARAM) {
        return (odin3_ast_id){0}; /* P7: ignored (warned by the scan), not in the elaborated form */
    }
    uint16_t flags = odin3_ast_flags(wr->ast, group) &
                     (uint16_t)(ODIN3_AST_F_PARAM_DECL_SIGNED | ODIN3_AST_F_PARAM_DECL_DT_MASK);
    flags |= in_header ? (uint16_t)ODIN3_AST_F_PARAM_DECL_IN_HEADER : 0;
    uint32_t mark = w_mark(wr);
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
    w_declarators(wr, group, w_param_declarator);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_PARAM_DECL, group);
    spec.sub = ODIN3_AST_PARAM_DECL_LOCALPARAM;
    spec.flags = flags;
    return w_make_marked(wr, &spec, mark, group);
}

/* The RANGE of an `input integer` treated as a net (M12): [31:0]. */
static odin3_ast_id w_integer_range(e1w *wr, odin3_ast_id from) {
    uint32_t mark = w_mark(wr);
    w_push(wr, w_int(wr, E1_INT_BITS - 1, from));
    w_push(wr, w_int(wr, 0, from));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_RANGE, from);
    return w_make_marked(wr, &spec, mark, from);
}

/* One merged port (pass E: odin3_elab1_ports): PORT_DECL{IN_HEADER} with one DECLARATOR; the NET
 * field is never 0 on a net port (EL-24). */
static odin3_ast_id w_port(e1w *wr, const odin3_elab1_mport *port) {
    uint32_t flags = (uint32_t)ODIN3_AST_F_PORT_DECL_IN_HEADER |
                     (port->is_signed ? (uint32_t)ODIN3_AST_F_PORT_DECL_SIGNED : 0U) |
                     ((uint32_t)port->dt << (uint32_t)ODIN3_AST_F_PORT_DECL_DT_SHIFT) |
                     ((uint32_t)port->net << (uint32_t)ODIN3_AST_F_PORT_DECL_NET_SHIFT);
    uint32_t mark = w_mark(wr);
    w_push(wr, port->as_integer && port->range.v == 0 ? w_integer_range(wr, port->decl)
                                                      : w_range(wr, port->range));
    bool own_value = odin3_ast_child(wr->ast, port->decl, 0).v != 0;
    w_push(wr, w_declarator_valued(
                   wr, &(w_declarator_of){port->decl, own_value ? port->decl : port->obj_decl}));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_PORT_DECL, port->group);
    spec.sub = port->dir;
    spec.flags = (uint16_t)flags;
    odin3_ast_id made = w_make_plain(wr, &spec, mark, port->group);
    w_attach_attrs(wr, &(w_attrs_of){.from = port->obj_first ? port->obj_group : port->group,
                                     .made = made,
                                     .then = port->obj_first ? port->group : port->obj_group});
    return made;
}

/* The ports LIST: one PORT_DECL per merged port (pass E's list); empty positions are EL-10's. */
static odin3_ast_id w_ports(e1w *wr, odin3_ast_id list) {
    uint32_t mark = w_mark(wr);
    for (size_t i = 0; i < wr->env->ports.len; i++) {
        const odin3_elab1_mport *port = odin3_vec_cat(&wr->env->ports, i);
        if (port->name != 0) {
            w_push(wr, w_port(wr, port));
        }
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list);
    return w_make_marked(wr, &spec, mark, list);
}

/* Whether decl (a declarator of a NET_DECL or VAR_DECL) was merged into a body port (EL-24). */
static bool merged_decl(const e1w *wr, odin3_ast_id decl) {
    if (wr->scope != 0) {
        return false;
    }
    uint32_t idx = odin3_elab1_env_find(wr->env, 0, odin3_ast_name(wr->ast, decl));
    const odin3_elab1_slot *slot = idx != E1_NONE ? odin3_elab1_env_slot(wr->env, idx) : NULL;
    return slot != NULL && slot->group2.v != 0 &&
           (slot->entry.decl.v == decl.v || slot->decl2.v == decl.v);
}

/* The declarators of group not merged into a port. */
static uint32_t unmerged_count(const e1w *wr, odin3_ast_id group) {
    odin3_ast_span kids = odin3_ast_children(wr->ast, group);
    uint32_t count = 0;
    for (uint32_t i = odin3_ast_slot_count(odin3_ast_kind_of(wr->ast, group)); i < kids.n; i++) {
        count += merged_decl(wr, kids.ids[i]) ? 0U : 1U;
    }
    return count;
}

/* w_declarators without the declarators merged into ports. */
static void w_object_declarators(e1w *wr, odin3_ast_id group) {
    odin3_ast_span kids = odin3_ast_children(wr->ast, group);
    for (uint32_t i = odin3_ast_slot_count(odin3_ast_kind_of(wr->ast, group)); i < kids.n; i++) {
        if (!merged_decl(wr, kids.ids[i])) {
            w_push(wr, w_declarator(wr, kids.ids[i]));
        }
    }
}

/* NET_DECL with NUMBER bounds, no strength, delay, `vectored` or `scalared` (EL-4). */
static odin3_ast_id w_net_decl(e1w *wr, odin3_ast_id group) {
    uint32_t sub = odin3_ast_sub(wr->ast, group);
    if (sub == ODIN3_AST_NET_DECL_TRI0 || sub == ODIN3_AST_NET_DECL_TRI1 ||
        sub == ODIN3_AST_NET_DECL_TRIREG) {
        (void)odin3_elab1_reject(wr->e1, group); /* N15 */
        halt(wr);
        return (odin3_ast_id){0};
    }
    if (unmerged_count(wr, group) == 0) {
        return (odin3_ast_id){0}; /* every declarator is a port's (EL-24) */
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, E1_NET_RANGE_SLOT)));
    w_push(wr, (odin3_ast_id){0});
    w_object_declarators(wr, group);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NET_DECL, group);
    spec.sub = (uint8_t)sub;
    spec.flags = odin3_ast_flags(wr->ast, group) & (uint16_t)ODIN3_AST_F_NET_DECL_SIGNED;
    return w_make_marked(wr, &spec, mark, group);
}

/* VAR_DECL[reg|integer] with NUMBER bounds (time, real and realtime are N17's, Task 16). */
static odin3_ast_id w_var_decl(e1w *wr, odin3_ast_id group) {
    uint32_t sub = odin3_ast_sub(wr->ast, group);
    if (sub != ODIN3_AST_VAR_DECL_REG && sub != ODIN3_AST_VAR_DECL_INTEGER) {
        (void)odin3_elab1_reject(wr->e1, group); /* N17 */
        halt(wr);
        return (odin3_ast_id){0};
    }
    if (unmerged_count(wr, group) == 0) {
        return (odin3_ast_id){0}; /* every declarator is a port's (EL-24) */
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
    w_object_declarators(wr, group);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_VAR_DECL, group);
    spec.sub = (uint8_t)sub;
    spec.flags = odin3_ast_flags(wr->ast, group) & (uint16_t)ODIN3_AST_F_VAR_DECL_SIGNED;
    return w_make_marked(wr, &spec, mark, group);
}

static odin3_ast_id w_net_assign(e1w *wr, odin3_ast_id assign) {
    uint32_t mark = w_mark(wr);
    w_push(wr, w_expr(wr, odin3_ast_child(wr->ast, assign, 0)));
    w_push(wr, w_expr(wr, odin3_ast_child(wr->ast, assign, 1)));
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NET_ASSIGN, assign);
    return w_make_marked(wr, &spec, mark, assign);
}

/* CONT_ASSIGN without its STRENGTH and DELAY slots (EL-4). */
static odin3_ast_id w_cont_assign(e1w *wr, odin3_ast_id item) {
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    w_push(wr, (odin3_ast_id){0});
    w_declarators(wr, item, w_net_assign);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_CONT_ASSIGN, item);
    odin3_ast_id id = w_make_plain(wr, &spec, mark, item);
    w_attrs_of at = {.from = item, .made = id};
    if (wr->e1->opts.netlist) {
        odin3_netlist_assign_src where = {item, wr->next_item};
        (void)odin3_netlist_assign_comment(wr->e1->design, wr->ast, &where, &at.loc_text);
    }
    w_attach_attrs(wr, &at);
    return id;
}

/* --- the decision log (§3.4; internal risk §16 #4) ---------------------------------------- */

/* The decision pass E logged for node, in order; ODIN3_DECISION_SKIPPED and a failed replay when
 * the next entry is not node's. The test hook drop_log_entry skips the Nth entry of each log. */
static int32_t w_replay(e1w *wr, odin3_ast_id node) {
    const odin3_elab_test_hooks *hooks = odin3_elab_test_hooks_get();
    uint32_t end = wr->spec->first_log + wr->spec->n_log;
    if (!wr->dropped && hooks->drop_log_entry != 0 && wr->cursor < end &&
        wr->cursor - wr->spec->first_log + 1 == hooks->drop_log_entry) {
        wr->cursor++;
        wr->dropped = true;
    }
    const odin3_elab_decision *entry =
        wr->cursor < end ? odin3_vec_cat(&wr->e1->log, wr->cursor) : NULL;
    if (entry == NULL || entry->node.v != node.v) {
        wr->replay_failed = true;
        return ODIN3_DECISION_SKIPPED;
    }
    wr->cursor++;
    wr->used++;
    return entry->value;
}

static odin3_status w_replay_done(e1w *wr) {
    if (!wr->replay_failed && wr->used == wr->spec->n_log) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR,
              "stage 1: decision log of '%s' not replayed (%" PRIu32 " of %" PRIu32 " entries)",
              odin3_elab1_str(wr->e1, wr->spec->name), wr->used, wr->spec->n_log);
    wr->e1->fatal = wr->e1->fatal != ODIN3_OK ? wr->e1->fatal : ODIN3_ERR_CHECK;
    return ODIN3_ERR_CHECK;
}

/* --- instances ------------------------------------------------------------------------------ */

/* A CONNECTION to write: its name (0: ordered), its value (absent when 0), its location. */
typedef struct e1w_conn {
    uint32_t name;
    odin3_ast_id value;
    odin3_ast_id from;
} e1w_conn;

static odin3_ast_id w_connection(e1w *wr, const e1w_conn *conn) {
    uint32_t mark = w_mark(wr);
    w_push(wr, conn->value);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_CONNECTION, conn->from);
    spec.name = conn->name;
    return w_make_marked(wr, &spec, mark, conn->from);
}

/* The params LIST of a module instance: one named CONNECTION per parameter of the
 * specialization, in declaration order, with its final value (absent without parameters). */
static odin3_ast_id w_spec_params(e1w *wr, odin3_ast_id item, uint32_t child) {
    const odin3_elab1_spec *spec = odin3_elab1_spec_at(wr->e1, child);
    const odin3_elab_def *def = odin3_elab1_def(wr->e1, spec->def);
    if (spec->n_values == 0) {
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    for (uint32_t k = 0; k < spec->n_values; k++) {
        odin3_ast_id decl =
            *(const odin3_ast_id *)odin3_vec_cat(&wr->e1->params, def->first_param + k);
        e1w_conn conn = {odin3_ast_name(wr->ast, decl),
                         w_value(wr, &spec->values[k], spec->bases[k], item), item};
        w_push(wr, w_connection(wr, &conn));
    }
    odin3_ast_spec list = spec_of(wr, ODIN3_AST_LIST, item);
    return w_make_marked(wr, &list, mark, item);
}

/* A cell type's INT parameter (EL-8): the 32-bit integer form when it fits, else 64 signed bits. */
static odin3_ast_id w_int_param(e1w *wr, int64_t num, odin3_ast_id from) {
    if (num >= INT32_MIN && num <= INT32_MAX) {
        return w_int(wr, num, from);
    }
    odin3_cval_int wide = {num, E1_INT64_BITS, true};
    odin3_cval val = {0};
    odin3_status st = odin3_cval_from_i64(wr->env->arena, &wide, &val);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    return w_value(wr, &val, ODIN3_AST_NUMBER_BIN, from);
}

/* The params LIST of a cell-type or implicit black-box instance from its record (EL-4, EL-8):
 * absent when nothing was given. */
static odin3_ast_id w_record_params(e1w *wr, odin3_ast_id item, uint32_t record) {
    const odin3_elab1_given *rec = odin3_vec_cat(&wr->e1->given, record);
    if (rec->count == 0 && odin3_ast_child(wr->ast, item, 0).v == 0) {
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    for (uint32_t i = 0; i < rec->count; i++) {
        odin3_elab1_gparam gp =
            *(const odin3_elab1_gparam *)odin3_vec_cat(&wr->e1->gparams, rec->first + i);
        odin3_ast_id from = gp.ovr.from.v != 0 ? gp.ovr.from : item;
        odin3_ast_id value = !gp.as_given && gp.kind == ODIN3_VAL_INT
                                 ? w_int_param(wr, gp.num, from)
                                 : w_value(wr, &gp.ovr.value, gp.ovr.base, from);
        e1w_conn conn = {gp.name, value, from};
        w_push(wr, w_connection(wr, &conn));
    }
    odin3_ast_id list = odin3_ast_child(wr->ast, item, 0);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list.v != 0 ? list : item);
    return w_make_marked(wr, &spec, mark, list.v != 0 ? list : item);
}

/* --- array slices (EL-10: element i gets bits [|i − right| × P +: P]) --------------------- */

/* A SELECT of obj: the bit idx_hi when bit, else the part [idx_hi:idx_lo], bounds NUMBERs. */
typedef struct e1w_sel {
    odin3_ast_id obj; /* written already */
    int64_t hi;
    int64_t lo;
    bool bit;
} e1w_sel;

static odin3_ast_id w_select(e1w *wr, const e1w_sel *sel, odin3_ast_id from) {
    uint32_t mark = w_mark(wr);
    w_push(wr, sel->obj);
    w_push(wr, w_int(wr, sel->hi, from));
    odin3_ast_id lsb = {0};
    if (!sel->bit) {
        lsb = w_int(wr, sel->lo, from);
    }
    w_push(wr, lsb);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_SELECT, from);
    spec.sub = sel->bit ? ODIN3_AST_SELECT_BIT : ODIN3_AST_SELECT_PART;
    return w_make_marked(wr, &spec, mark, from);
}

/* A slice to write: the bits [off, off + width) of expr's value (bit 0 its LSB). */
typedef struct e1w_slice {
    odin3_ast_id expr;
    uint64_t off;
    uint32_t width;
} e1w_slice;

/* A net or variable (not a memory) named by ident: its declared range in *msb, *lsb. */
static bool plain_signal(e1w *wr, odin3_ast_id ident, int64_t *msb, int64_t *lsb) {
    uint32_t idx = odin3_elab1_env_lookup(wr->env, odin3_ast_name(wr->ast, ident));
    const odin3_elab1_slot *slot = idx != E1_NONE ? odin3_elab1_env_slot(wr->env, idx) : NULL;
    if (slot == NULL || slot->entry.kind != ODIN3_ENTRY_SIGNAL || slot->n_unpacked != 0 ||
        slot->state != ODIN3_E1_DONE) {
        return false;
    }
    *msb = slot->msb;
    *lsb = slot->lsb;
    return true;
}

/* The slice of a select of ident whose value's bit k is ident[right + k × dir]. */
typedef struct e1w_rebase {
    odin3_ast_id ident;
    int64_t right;
    int64_t dir;
} e1w_rebase;

static odin3_ast_id w_rebased(e1w *wr, const e1w_rebase *rb, const e1w_slice *sl) {
    int64_t off = (int64_t)sl->off;
    e1w_sel sel = {w_expr(wr, rb->ident), rb->right + (off + sl->width - 1) * rb->dir,
                   rb->right + off * rb->dir, sl->width == 1};
    return w_select(wr, &sel, sl->expr);
}

/* A name's slice in its declared indices (its LSB is its right index); 0 when not a plain net. */
static odin3_ast_id w_slice_ident(e1w *wr, const e1w_slice *sl) {
    int64_t msb = 0;
    int64_t lsb = 0;
    if (!plain_signal(wr, sl->expr, &msb, &lsb)) {
        return (odin3_ast_id){0};
    }
    e1w_rebase rb = {sl->expr, lsb, msb >= lsb ? 1 : -1};
    return w_rebased(wr, &rb, sl);
}

/* A constant bound of a select, quietly (a variable +: base is not one). */
static bool quiet_int(e1w *wr, odin3_ast_id expr, int64_t *out) {
    odin3_cval val = {0};
    bool is_const = false;
    odin3_status st = odin3_elab1_try_constant(wr->e1, expr, &val, &is_const);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
    }
    return st == ODIN3_OK && is_const && odin3_cval_to_i64(&val, out);
}

/* The bounds [left:right] of a constant part select (+: and -: in its name's direction). */
static bool part_bounds(e1w *wr, odin3_ast_id sel, bool down, int64_t bounds[2]) {
    int64_t first = 0;
    int64_t second = 0;
    if (!quiet_int(wr, odin3_ast_child(wr->ast, sel, 1), &first) ||
        !quiet_int(wr, odin3_ast_child(wr->ast, sel, 2), &second)) {
        return false;
    }
    int64_t span = second - 1;
    switch (odin3_ast_sub(wr->ast, sel)) {
    case ODIN3_AST_SELECT_PART:
        bounds[0] = first;
        bounds[1] = second;
        return true;
    case ODIN3_AST_SELECT_PART_PLUS: /* [b+w-1:b] or [b:b+w-1] */
        bounds[0] = down ? first + span : first;
        bounds[1] = down ? first : first + span;
        return true;
    case ODIN3_AST_SELECT_PART_MINUS: /* [b:b-w+1] or [b-w+1:b] */
        bounds[0] = down ? first : first - span;
        bounds[1] = down ? first - span : first;
        return true;
    default:
        return false;
    }
}

/* A constant part select of a plain net, rebased; 0 when it is not one. */
static odin3_ast_id w_slice_select(e1w *wr, const e1w_slice *sl) {
    odin3_ast_id base = odin3_ast_child(wr->ast, sl->expr, 0);
    int64_t msb = 0;
    int64_t lsb = 0;
    int64_t bounds[2] = {0, 0};
    if (odin3_ast_kind_of(wr->ast, base) != ODIN3_AST_IDENT ||
        odin3_ast_sub(wr->ast, sl->expr) == ODIN3_AST_SELECT_BIT ||
        !plain_signal(wr, base, &msb, &lsb) || !part_bounds(wr, sl->expr, msb >= lsb, bounds)) {
        return (odin3_ast_id){0};
    }
    e1w_rebase rb = {base, bounds[1], bounds[0] >= bounds[1] ? 1 : -1};
    return w_rebased(wr, &rb, sl);
}

/* A constant's slice as a NUMBER (sized, unsigned); 0 when expr is not a constant. */
static odin3_ast_id w_slice_const(e1w *wr, const e1w_slice *sl) {
    odin3_cval val = {0};
    bool is_const = false;
    odin3_status st = odin3_elab1_try_constant(wr->e1, sl->expr, &val, &is_const);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
    }
    if (st != ODIN3_OK || !is_const) {
        return (odin3_ast_id){0};
    }
    odin3_cval_part part = {&val, (int64_t)sl->off, sl->width};
    odin3_cval bits = {0};
    st = odin3_cval_select(wr->env->arena, &part, &bits);
    if (st != ODIN3_OK) {
        fail(wr, st);
        return (odin3_ast_id){0};
    }
    return w_value(wr, &bits, ODIN3_AST_NUMBER_BIN, sl->expr);
}

/* One leaf's slice: a name, a constant part select, a constant, else a SELECT of the
 * expression's own value (bit 0 its LSB; stage 2 reads such a select so, EL-10). */
static odin3_ast_id w_slice_leaf(e1w *wr, const e1w_slice *sl) {
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, sl->expr);
    odin3_ast_id made = {0};
    if (kind == ODIN3_AST_IDENT) {
        made = w_slice_ident(wr, sl);
    } else if (kind == ODIN3_AST_SELECT) {
        made = w_slice_select(wr, sl);
    }
    if (made.v == 0 && live(wr) && kind != ODIN3_AST_CONCAT) {
        made = w_slice_const(wr, sl);
    }
    if (made.v != 0 || !live(wr)) {
        return made;
    }
    int64_t off = (int64_t)sl->off;
    e1w_sel sel = {w_expr(wr, sl->expr), off + sl->width - 1, off, sl->width == 1};
    return w_select(wr, &sel, sl->expr);
}

/* A concatenation's slice: the slices of the elements it covers, MSB first (one alone). */
static odin3_ast_id w_slice_concat(e1w *wr, const e1w_slice *sl) {
    odin3_ast_span kids = odin3_ast_children(wr->ast, sl->expr);
    uint64_t total = 0;
    for (uint32_t i = 0; i < kids.n; i++) {
        total += odin3_elab1_typed_width(wr->e1, kids.ids[i]);
    }
    uint32_t mark = w_mark(wr);
    uint32_t pieces = 0;
    odin3_ast_id last = {0};
    uint64_t end = sl->off + sl->width;
    for (uint32_t i = 0; i < kids.n; i++) {
        uint32_t width = odin3_elab1_typed_width(wr->e1, kids.ids[i]);
        uint64_t lo = total - width;
        total = lo;
        uint64_t from = sl->off > lo ? sl->off : lo;
        uint64_t to = end < lo + width ? end : lo + width;
        if (from >= to) {
            continue;
        }
        e1w_slice part = {kids.ids[i], from - lo, (uint32_t)(to - from)};
        last = part.width == width ? w_expr(wr, kids.ids[i]) : w_slice_leaf(wr, &part);
        w_push(wr, last);
        pieces++;
    }
    if (pieces == 1) {
        if (live(wr)) {
            odin3_ast_unwind(wr->out, mark);
        }
        return last;
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_CONCAT, sl->expr);
    return w_make_marked(wr, &spec, mark, sl->expr);
}

static odin3_ast_id w_slice(e1w *wr, const e1w_slice *sl) {
    if (odin3_ast_kind_of(wr->ast, sl->expr) == ODIN3_AST_CONCAT) {
        return w_slice_concat(wr, sl);
    }
    return w_slice_leaf(wr, sl);
}

/* --- instances: elements and connections (EL-10) ------------------------------------------ */

/* One INSTANCE to write: its name, whether its connections are named by port (a module's), and
 * for an array element its index (bit offsets |index − right| × P). */
typedef struct e1w_elem {
    odin3_ast_id inst;
    uint32_t name;
    bool by_port;
    bool element;
    int64_t index;
} e1w_elem;

static odin3_ast_id w_conn_value(e1w *wr, const odin3_elab1_conn *rec, const e1w_elem *el) {
    if (rec->expr.v == 0) {
        return (odin3_ast_id){0};
    }
    if (!el->element || !rec->sliced) {
        return w_expr(wr, rec->expr);
    }
    const odin3_elab1_array *arr = &wr->e1->array;
    int64_t low = arr->left < arr->right ? arr->left : arr->right;
    uint64_t step = (uint64_t)(el->index - low); /* the lowest index the low bits (EL-10) */
    e1w_slice sl = {rec->expr, step * rec->port_width, rec->port_width};
    return w_slice(wr, &sl);
}

static odin3_ast_id w_element(e1w *wr, const e1w_elem *el) {
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    for (size_t i = 0; i < wr->e1->conns.len && wr->st == ODIN3_OK; i++) {
        odin3_elab1_conn rec = *(const odin3_elab1_conn *)odin3_vec_cat(&wr->e1->conns, i);
        e1w_conn out = {el->by_port ? rec.name : odin3_ast_name(wr->ast, rec.conn),
                        w_conn_value(wr, &rec, el), rec.conn.v != 0 ? rec.conn : el->inst};
        w_push(wr, w_connection(wr, &out));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_INSTANCE, el->inst);
    spec.name = el->name;
    return w_make_marked(wr, &spec, mark, el->inst);
}

/* The name of element index of the array instance inst: "u[3]" (EL-4, Yosys's names). */
static uint32_t element_name(e1w *wr, odin3_ast_id inst, int64_t index) {
    char text[ODIN3_LOG_BUF];
    int got =
        snprintf(text, sizeof text, "%s[%" PRId64 "]", odin3_ast_name_str(wr->ast, inst), index);
    uint32_t name = 0;
    odin3_status st = got > 0 && (size_t)got < sizeof text
                          ? odin3_ast_intern(wr->out, (odin3_bytes){text, (size_t)got}, &name)
                          : ODIN3_ERR_CHECK;
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
    return name;
}

/* The INSTANCE nodes of inst: its connections resolved and checked (EL-10), then one INSTANCE,
 * or one per array element from left to right, sliced as planned. */
static void w_instances(e1w *wr, const odin3_elab_inst *inst, bool by_port) {
    odin3_status st = odin3_elab1_connect(wr->e1, inst);
    bool array = odin3_ast_child(wr->ast, inst->instance, 0).v != 0;
    st = st != ODIN3_OK || !array ? st : odin3_elab1_inst_array(wr->e1, inst);
    if (st != ODIN3_OK) {
        stop_on(wr, st);
        return;
    }
    e1w_elem el = {inst->instance, odin3_ast_name(wr->ast, inst->instance), by_port, false, 0};
    if (!array) {
        w_push(wr, w_element(wr, &el));
        return;
    }
    const odin3_elab1_array *arr = &wr->e1->array;
    int64_t dir = arr->left >= arr->right ? -1 : 1;
    el.element = true;
    uint32_t count = live(wr) ? arr->n : 1; /* not building: one walk reports what is pending */
    for (uint32_t k = 0; k < count && wr->st == ODIN3_OK; k++) {
        el.index = arr->left + dir * (int64_t)k;
        el.name = element_name(wr, inst->instance, el.index);
        w_push(wr, w_element(wr, &el));
    }
}

/* --- INSTANTIATIONs ------------------------------------------------------------------------- */

/* The INSTANTIATIONs written for item so far: consecutive instances of one decision (one
 * specialization, or one parameter record) share one (EL-4: its name is the specialization's). */
typedef struct e1w_group {
    bool open;
    int32_t value;
    uint32_t mark;
} e1w_group;

static void close_group(e1w *wr, odin3_ast_id item, e1w_group *group) {
    if (!group->open) {
        return;
    }
    group->open = false;
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_INSTANTIATION, item);
    spec.name = group->value >= 0 ? odin3_elab1_spec_at(wr->e1, (uint32_t)group->value)->name
                                  : odin3_ast_name(wr->ast, item);
    w_push(wr, w_make_marked(wr, &spec, group->mark, item));
}

/* The instance a decision describes (its target and specialization or record). */
static odin3_elab_inst decided(const e1w *wr, odin3_ast_id item, odin3_ast_id kid, int32_t value) {
    odin3_elab_inst inst = {item, kid, 0, {ODIN3_RES_NONE, 0, {0}}, E1_NONE};
    if (value >= 0) {
        inst.target.kind = ODIN3_RES_MODULE;
        inst.target.def = odin3_elab1_spec_at(wr->e1, (uint32_t)value)->def;
        inst.spec = (uint32_t)value;
    } else {
        inst.spec = odin3_elab1_given_of(value);
        inst.target = ((const odin3_elab1_given *)odin3_vec_cat(&wr->e1->given, inst.spec))->target;
    }
    return inst;
}

/* The params LIST of a new INSTANTIATION group: a specialization's final values (a module's or a
 * `(* blackbox *)` definition's), or a cell type's or implicit black box's record. */
static odin3_ast_id w_group_params(e1w *wr, odin3_ast_id item, const odin3_elab_inst *inst) {
    if (inst->target.kind == ODIN3_RES_MODULE) {
        return w_spec_params(wr, item, inst->spec);
    }
    return w_record_params(wr, item, inst->spec);
}

static void w_instantiation(e1w *wr, odin3_ast_id item) {
    e1w_group group = {false, 0, 0};
    odin3_ast_span kids = odin3_ast_children(wr->ast, item);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); i < kids.n; i++) {
        int32_t value = w_replay(wr, kids.ids[i]);
        if (value == ODIN3_DECISION_SKIPPED) {
            halt(wr); /* reported in pass E, or a failed replay */
            continue;
        }
        odin3_elab_inst inst = decided(wr, item, kids.ids[i], value);
        bool by_port = inst.target.kind != ODIN3_RES_CELLTYPE; /* EL-4 v4.8 */
        if (group.open && value != group.value) {
            close_group(wr, item, &group);
        }
        if (!group.open) {
            group = (e1w_group){true, value, w_mark(wr)};
            w_push(wr, w_group_params(wr, item, &inst));
        }
        w_instances(wr, &inst, by_port);
    }
    close_group(wr, item, &group);
}

/* The switch-level and pull gates (Q6) are Task 16's REJECTs. */
static bool switch_level(uint32_t sub) {
    return sub >= ODIN3_AST_GATE_DECL_NMOS;
}

/* GATE_DECL without STRENGTH and DELAY (EL-4), its arrays expanded (P = 1), unnamed instances
 * keeping name 0. */
static odin3_ast_id w_gate(e1w *wr, odin3_ast_id item) {
    uint32_t sub = odin3_ast_sub(wr->ast, item);
    if (switch_level(sub)) {
        (void)odin3_elab1_reject(wr->e1, item); /* Q6 */
        halt(wr);
        return (odin3_ast_id){0};
    }
    uint32_t mark = w_mark(wr);
    w_push(wr, (odin3_ast_id){0});
    w_push(wr, (odin3_ast_id){0});
    odin3_ast_span kids = odin3_ast_children(wr->ast, item);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_GATE_DECL); i < kids.n; i++) {
        odin3_elab_inst inst = {item, kids.ids[i], 0, {ODIN3_RES_NONE, 0, {0}}, E1_NONE};
        w_instances(wr, &inst, false);
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_GATE_DECL, item);
    spec.sub = (uint8_t)sub;
    return w_make_marked(wr, &spec, mark, item);
}

/* --- generate blocks (Task 14) --------------------------------------------------------------- */

/* One frame of the module's walk: the items [next, end) of node in scope; a generate block (its
 * GEN_BLOCK made from mark once its items are written) fresh until its index slot is pushed. */
typedef struct e1w_frame {
    odin3_ast_id node;
    uint32_t next;
    uint32_t end;
    uint32_t scope;
    bool block;
    bool fresh;
    uint32_t mark;
    odin3_ast_id from; /* the block's source: its GEN_BLOCK, bare item or NULL_STMT */
} e1w_frame;

static void w_frame(e1w *wr, const e1w_frame *frame) {
    e1w_frame *slot = odin3_vec_push(&wr->walk);
    if (slot == NULL) {
        fail(wr, ODIN3_ERR_NO_MEMORY);
        return;
    }
    *slot = *frame;
}

/* The selected blocks of a construct: pass E's scopes [first, first + n), walked in order. */
static void w_blocks(e1w *wr, uint32_t first, uint32_t n) {
    for (uint32_t k = n; k > 0 && wr->st == ODIN3_OK; k--) {
        uint32_t scope = first + k - 1;
        const odin3_elab1_scope_x *ext = odin3_elab1_scope_ext(wr->env, scope);
        if (ext == NULL) {
            wr->replay_failed = true; /* pass E made no such scope */
            continue;
        }
        e1w_frame frame = {.scope = scope, .block = true, .fresh = true};
        frame.from = odin3_ast_child(wr->ast, ext->holder, ext->slot);
        frame.from = frame.from.v != 0 ? frame.from : ext->holder;
        odin3_elab1_scope_items(wr->env, scope, &frame.node, &frame.next, &frame.end);
        w_frame(wr, &frame);
    }
}

/* A GEN_IF or GEN_CASE replayed: its selected block, if any. */
static void w_choice(e1w *wr, odin3_ast_id item) {
    if (w_replay(wr, item) >= 0) {
        w_blocks(wr, wr->next_scope++, 1);
    }
}

/* A GEN_FOR replayed: its count, its values, then one block per iteration. */
static void w_loop(e1w *wr, odin3_ast_id item) {
    int32_t logged = w_replay(wr, item);
    uint32_t count = logged > 0 ? (uint32_t)logged : 0;
    for (uint32_t k = 0; k < count; k++) {
        (void)w_replay(wr, item);
    }
    w_blocks(wr, wr->next_scope, count);
    wr->next_scope += count;
}

/* A generate item: a region's items queued in the scope, a construct's selected blocks, a genvar
 * declaration dropped (EL-4). False for the other kinds. */
static bool w_generate(e1w *wr, odin3_ast_id item) {
    switch (odin3_ast_kind_of(wr->ast, item)) {
    case ODIN3_AST_GENERATE: {
        e1w_frame frame = {
            .node = item, .end = odin3_ast_nchild(wr->ast, item), .scope = wr->scope, .from = item};
        w_frame(wr, &frame);
        return true;
    }
    case ODIN3_AST_GEN_FOR:
        w_loop(wr, item);
        return true;
    case ODIN3_AST_GEN_IF:
    case ODIN3_AST_GEN_CASE:
        w_choice(wr, item);
        return true;
    case ODIN3_AST_GENVAR_DECL:
        return true;
    default:
        return false;
    }
}

/* A fresh block: its index slot (a loop iteration's value, else absent) on the pending stack. */
static void w_open_block(e1w *wr, e1w_frame *frame) {
    frame->fresh = false;
    frame->mark = w_mark(wr);
    const odin3_elab_scope *rec = odin3_elab1_scope_at(wr->env, frame->scope);
    w_push(wr, rec->has_index ? w_int(wr, rec->index, frame->from) : (odin3_ast_id){0});
}

static void w_close_block(e1w *wr, const e1w_frame *frame) {
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_GEN_BLOCK, frame->from);
    spec.name = odin3_elab1_scope_at(wr->env, frame->scope)->name;
    w_push(wr, w_make_marked(wr, &spec, frame->mark, frame->from));
}

/* --- the module ----------------------------------------------------------------------------- */

/* T6 (accepted): an `always` whose statement is not led by an event control (none at all, or a
 * delay, which is dropped as T11) is written as `always @*`: ALWAYS(TIMING_STMT(EVENT_CONTROL
 * star, body)), the one form IR-checked ALWAYS nodes have. */
static odin3_ast_id w_always_star(e1w *wr, odin3_ast_id item, odin3_ast_id first) {
    uint32_t outer = w_mark(wr);
    uint32_t inner = w_mark(wr);
    odin3_ast_id from = first.v != 0 ? first : item;
    odin3_ast_spec control = spec_of(wr, ODIN3_AST_EVENT_CONTROL, from);
    control.flags = ODIN3_AST_F_EVENT_CONTROL_STAR;
    w_push(wr, (odin3_ast_id){0}); /* the control's optional expression slot */
    w_push(wr, w_make_plain(wr, &control, inner, from));
    w_push(wr, first.v != 0 ? w_expr(wr, first) : w_null_make(wr, item));
    odin3_ast_spec timing = spec_of(wr, ODIN3_AST_TIMING_STMT, from);
    w_push(wr, w_make_plain(wr, &timing, inner, from));
    odin3_ast_spec always = spec_of(wr, ODIN3_AST_ALWAYS, item);
    always.sub = (uint8_t)odin3_ast_sub(wr->ast, item);
    always.name = odin3_ast_name(wr->ast, item);
    return w_make_marked(wr, &always, outer, item);
}

/* Whether an ALWAYS (the plain `always` keyword) lacks an event control (T6). */
static bool always_untimed(const e1w *wr, odin3_ast_id item) {
    odin3_ast_id first = odin3_ast_child(wr->ast, item, 0);
    return odin3_ast_kind_of(wr->ast, item) == ODIN3_AST_ALWAYS &&
           odin3_ast_sub(wr->ast, item) == ODIN3_AST_ALWAYS_ALWAYS &&
           !(first.v != 0 && odin3_ast_kind_of(wr->ast, first) == ODIN3_AST_TIMING_STMT &&
             odin3_ast_kind_of(wr->ast, odin3_ast_child(wr->ast, first, 0)) ==
                 ODIN3_AST_EVENT_CONTROL);
}

/* ALWAYS and INITIAL: the statement tree copied (T6: an always without an event control is
 * written as `always @*`, the warning being the scan's). */
static odin3_ast_id w_process(e1w *wr, odin3_ast_id item) {
    if (always_untimed(wr, item)) {
        return w_always_star(wr, item, odin3_ast_child(wr->ast, item, 0));
    }
    wr->in_initial = odin3_ast_kind_of(wr->ast, item) == ODIN3_AST_INITIAL;
    odin3_ast_id made = w_expr(wr, item);
    wr->in_initial = false;
    return made;
}

/* One PORT_DECL per declarator of a function's or task's port group (EL-4). */
static void w_sub_port(e1w *wr, odin3_ast_id group) {
    odin3_ast_span decls = odin3_ast_children(wr->ast, group);
    for (uint32_t j = odin3_ast_slot_count(ODIN3_AST_PORT_DECL); j < decls.n; j++) {
        uint32_t mark = w_mark(wr);
        w_push(wr, w_range(wr, odin3_ast_child(wr->ast, group, 0)));
        w_push(wr, w_declarator(wr, decls.ids[j]));
        odin3_ast_spec spec = spec_of(wr, ODIN3_AST_PORT_DECL, group);
        spec.sub = (uint8_t)odin3_ast_sub(wr->ast, group);
        uint32_t flags =
            odin3_ast_flags(wr->ast, group) | (uint32_t)ODIN3_AST_F_PORT_DECL_IN_HEADER;
        if ((flags & (ODIN3_AST_F_PORT_DECL_DT_MASK | ODIN3_AST_F_PORT_DECL_NET_MASK)) == 0) {
            /* an untyped subroutine port is a wire, like a module port (EL-4, EL-24) */
            flags |= (1U + (uint32_t)ODIN3_AST_NET_DECL_WIRE) << ODIN3_AST_F_PORT_DECL_NET_SHIFT;
        }
        spec.flags = (uint16_t)flags;
        w_push(wr, w_make_marked(wr, &spec, mark, group));
    }
}

/* The PORT_DECLs (ports) or the other declarations of a function's or task's LIST. */
static void w_sub_entries(e1w *wr, odin3_ast_id list, bool ports) {
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; i < entries.n; i++) {
        odin3_ast_id group = entries.ids[i];
        odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, group);
        if (odin3_elab1_pending(wr->e1, group)) {
            halt(wr);
        } else if ((kind == ODIN3_AST_PORT_DECL) != ports) {
            continue;
        } else if (ports) {
            w_sub_port(wr, group);
        } else if (kind == ODIN3_AST_VAR_DECL) {
            w_push(wr, w_local_var(wr, group));
        } else if (kind == ODIN3_AST_PARAM_DECL) {
            w_push_item(wr, w_param_decl(wr, group, false));
        } else {
            w_local_other(wr, group);
        }
    }
}

/* A FUNCTION_DECL or TASK_DECL (EL-4): ports LIST (one declarator each, non-ANSI inputs moved
 * there in order), locals LIST, body. */
static odin3_ast_id w_subroutine(e1w *wr, odin3_ast_id node) {
    bool function = odin3_ast_kind_of(wr->ast, node) == ODIN3_AST_FUNCTION_DECL;
    uint32_t first = function ? 1U : 0U;
    odin3_ast_id ports = odin3_ast_child(wr->ast, node, first);
    odin3_ast_id locals = odin3_ast_child(wr->ast, node, first + 1);
    uint32_t mark = w_mark(wr);
    uint32_t outer = wr->env->cur_scope;
    w_scope(wr, node, true);
    if (function) {
        w_push(wr, w_range(wr, odin3_ast_child(wr->ast, node, 0)));
    }
    w_params_enter(wr, node);
    uint32_t list = w_mark(wr);
    w_sub_entries(wr, ports, true);
    w_sub_entries(wr, locals, true);
    odin3_ast_spec lspec = spec_of(wr, ODIN3_AST_LIST, ports);
    w_push(wr, w_make_marked(wr, &lspec, list, ports));
    list = w_mark(wr);
    w_sub_entries(wr, locals, false);
    lspec = spec_of(wr, ODIN3_AST_LIST, locals);
    w_push(wr, w_make_marked(wr, &lspec, list, locals));
    w_push(wr, w_expr(wr, odin3_ast_child(wr->ast, node, first + 2)));
    w_scope(wr, node, false);
    wr->env->cur_scope = outer;
    odin3_ast_spec spec = spec_of(wr, odin3_ast_kind_of(wr->ast, node), node);
    spec.name = odin3_ast_name(wr->ast, node);
    spec.flags = odin3_ast_flags(wr->ast, node);
    return w_make_marked(wr, &spec, mark, node);
}

/* The instances, gates, processes and subroutines of a body (the declarations are w_item's). */
static void w_behaviour(e1w *wr, odin3_ast_id item) {
    switch (odin3_ast_kind_of(wr->ast, item)) {
    case ODIN3_AST_CONT_ASSIGN:
        w_push(wr, w_cont_assign(wr, item));
        break;
    case ODIN3_AST_INSTANTIATION:
        w_instantiation(wr, item);
        break;
    case ODIN3_AST_GATE_DECL:
        w_push(wr, w_gate(wr, item));
        break;
    case ODIN3_AST_ALWAYS:
    case ODIN3_AST_INITIAL:
        w_push_item(wr, w_process(wr, item));
        break;
    case ODIN3_AST_FUNCTION_DECL:
    case ODIN3_AST_TASK_DECL:
        w_push(wr, w_subroutine(wr, item));
        break;
    default:
        odin3_elab1_unhandled(wr->e1, item);
        halt(wr);
        break;
    }
}

static void w_item(e1w *wr, odin3_ast_id item) {
    wr->env->cur_scope = wr->scope; /* a halted body may have left a parameter scope */
    odin3_ast_kind kind = odin3_ast_kind_of(wr->ast, item);
    if (kind == ODIN3_AST_DIRECTIVE || kind == ODIN3_AST_SPECIFY_BLOCK) {
        return; /* `default_nettype and `resetall: read by odin3_elab1_nettype_at; the rest ignored
                 */
    }
    if (kind == ODIN3_AST_EVENT_DECL) {
        (void)odin3_elab1_reject(wr->e1, item); /* N18 */
        halt(wr);
        return;
    }
    if (odin3_elab1_pending(wr->e1, item)) {
        halt(wr);
        return;
    }
    if (w_generate(wr, item)) {
        return;
    }
    switch (kind) {
    case ODIN3_AST_PARAM_DECL:
        w_push_item(wr, w_param_decl(wr, item, false));
        break;
    case ODIN3_AST_NET_DECL:
        w_push_item(wr, w_net_decl(wr, item));
        break;
    case ODIN3_AST_VAR_DECL:
        w_push_item(wr, w_var_decl(wr, item));
        break;
    case ODIN3_AST_DEFPARAM:
        break; /* resolved in pass E; absent from the elaborated form */
    case ODIN3_AST_PORT_DECL:
        if (wr->scope != 0) {
            halt(wr); /* in a generate block: reported in pass E */
        }
        break; /* a body port: merged into the ports LIST (EL-24) */
    default:
        w_behaviour(wr, item);
        break;
    }
}

/* The header's parameters LIST (absent when the source has none). */
static odin3_ast_id w_header_params(e1w *wr, odin3_ast_id list) {
    if (list.v == 0) {
        return list;
    }
    uint32_t mark = w_mark(wr);
    odin3_ast_span entries = odin3_ast_children(wr->ast, list);
    for (uint32_t i = 0; i < entries.n; i++) {
        if (odin3_ast_kind_of(wr->ast, entries.ids[i]) != ODIN3_AST_PARAM_DECL ||
            odin3_elab1_pending(wr->e1, entries.ids[i])) {
            odin3_elab1_not_yet(wr->e1, entries.ids[i]);
            halt(wr);
            continue;
        }
        w_push(wr, w_param_decl(wr, entries.ids[i], true));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_LIST, list);
    return w_make_marked(wr, &spec, mark, list);
}

/* The `implicit` ATTR of an implicit net's NET_DECL: how the binder knows it (EL-4, EL-5). */
static void w_implicit_attr(e1w *wr, const w_attrs_of *at) {
    odin3_ast_id net = at->made;
    static const char k_implicit[] = "implicit";
    odin3_ast_spec spec = {.kind = ODIN3_AST_ATTR,
                           .loc = odin3_ast_loc(wr->out, net).v,
                           .end = odin3_ast_end(wr->out, net).v};
    const odin3_ast_id none = {0};
    odin3_ast_id attr = {0};
    if (!live(wr) || !w_room(wr, at->from)) {
        return;
    }
    odin3_status st = odin3_ast_intern(wr->out, odin3_bytes_cstr(k_implicit), &spec.name);
    st = st != ODIN3_OK ? st : odin3_ast_make(wr->out, &spec, &none, 1, &attr);
    st = st != ODIN3_OK ? st : odin3_ast_attach(wr->out, net, &attr, 1);
    if (st != ODIN3_OK) {
        fail(wr, st);
    }
}

/* The foreign hierarchical references of the module as implicit nets, first-use order, each with
 * its warning (Yosys accepts them silently as undriven wires; Odin III says so). */
static void w_hier_nets(e1w *wr) {
    odin3_elab1_env *env = wr->env;
    for (size_t i = 0; i < env->hier_uses.len && live(wr); i++) {
        const odin3_elab1_hier_use *use = odin3_vec_cat(&env->hier_uses, i);
        uint32_t name = w_foreign_name(wr, use->node, use->scope);
        if (name == 0) {
            continue;
        }
        odin3_elab1_warn_once(wr->e1, use->node,
                              "hierarchical reference '%s' is not supported; it is read as an "
                              "undriven net of that name (as Yosys does)",
                              odin3_elab1_str(wr->e1, name));
        if (odin3_u64map_get(env->imp_names, name, NULL)) {
            continue;
        }
        odin3_elab1_implicit_net *rec = odin3_vec_push(&env->implicit);
        odin3_status st = rec == NULL ? ODIN3_ERR_NO_MEMORY
                                      : odin3_u64map_put(env->imp_names, (odin3_kv){name, 1});
        if (st != ODIN3_OK) {
            fail(wr, st);
            return;
        }
        *rec = (odin3_elab1_implicit_net){name, use->node};
    }
}

/* EL-4: the implicit nets as one NET_DECL first among the items, in first-use order, of the
 * `default_nettype kind in effect (EL-25). */
static void w_implicit(e1w *wr) {
    w_hier_nets(wr);
    const odin3_vec *nets = &wr->env->implicit;
    if (nets->len == 0) {
        return;
    }
    odin3_ast_id first = ((const odin3_elab1_implicit_net *)odin3_vec_cat(nets, 0))->use;
    uint32_t mark = w_mark(wr);
    for (uint32_t k = 0; k < 3; k++) {
        w_push(wr, (odin3_ast_id){0});
    }
    for (size_t i = 0; i < nets->len; i++) {
        const odin3_elab1_implicit_net *net = odin3_vec_cat(nets, i);
        uint32_t decl = w_mark(wr);
        w_push(wr, (odin3_ast_id){0}); /* no net declaration assignment */
        odin3_ast_spec dspec = spec_of(wr, ODIN3_AST_DECLARATOR, net->use);
        dspec.name = net->name;
        w_push(wr, w_make_plain(wr, &dspec, decl, net->use));
    }
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_NET_DECL, first);
    spec.sub = odin3_elab1_implicit_sub(wr->env->nettype);
    odin3_ast_id made = w_make_plain(wr, &spec, mark, first);
    w_implicit_attr(wr, &(w_attrs_of){.from = first, .made = made});
    w_push(wr, made);
}

/* The module's items, and those of every selected generate block, through a worklist. */
static void w_body(e1w *wr, odin3_ast_id module) {
    w_implicit(wr);
    e1w_frame top = {module, E1_ITEMS, odin3_ast_nchild(wr->ast, module), 0, false, false,
                     0,      module};
    w_frame(wr, &top);
    while (wr->walk.len > 0 && wr->st == ODIN3_OK && !wr->e1->stopped) {
        e1w_frame *frame = odin3_vec_at(&wr->walk, wr->walk.len - 1);
        e1w_frame now = *frame;
        wr->scope = now.scope;
        wr->env->cur_scope = now.scope;
        if (now.fresh) {
            w_open_block(wr, frame);
        } else if (now.next < now.end) {
            frame->next++;
            odin3_ast_span items = odin3_ast_children(wr->ast, now.node);
            wr->next_item = now.next + 1U < now.end ? items.ids[now.next + 1U] : (odin3_ast_id){0};
            w_item(wr, items.ids[now.next]);
        } else {
            odin3_vec_pop(&wr->walk);
            if (now.block) {
                w_close_block(wr, &now);
            }
        }
    }
    odin3_vec_clear(&wr->walk);
    wr->scope = 0;
    wr->env->cur_scope = 0;
}

static odin3_ast_id w_module(e1w *wr) {
    odin3_ast_id node = odin3_elab1_def(wr->e1, wr->spec->def)->node;
    uint32_t mark = w_mark(wr);
    w_push(wr, w_header_params(wr, odin3_ast_child(wr->ast, node, E1_SLOT_PARAMS)));
    odin3_ast_id ports = odin3_ast_child(wr->ast, node, E1_SLOT_PORTS);
    w_push(wr, ports.v != 0 ? w_ports(wr, ports) : ports);
    w_body(wr, node);
    odin3_ast_spec spec = spec_of(wr, ODIN3_AST_MODULE, node);
    spec.name = wr->spec->name;
    odin3_ast_id made = w_make_plain(wr, &spec, mark, node);
    w_attach_attrs(wr,
                   &(w_attrs_of){.from = node,
                                 .made = made,
                                 .unit_flags = odin3_elab1_def(wr->e1, wr->spec->def)->unit_flags});
    return made;
}

odin3_status odin3_elab1_write(odin3_elab1 *e1, uint32_t spec) {
    e1w wr = {.e1 = e1,
              .env = e1->env,
              .ast = e1->parsed,
              .out = e1->elab,
              .spec = odin3_elab1_spec_at(e1, spec),
              .building = e1->errors == 0 && e1->fatal == ODIN3_OK,
              .st = ODIN3_OK};
    if (wr.env == NULL || wr.out == NULL || wr.spec == NULL) {
        return ODIN3_ERR_CHECK; /* pass E did not run: a 2D bug */
    }
    wr.cursor = wr.spec->first_log;
    odin3_vec_init(&wr.walk, sizeof(e1w_frame));
    wr.next_scope = 1;
    wr.unnamed_ids = odin3_u64map_create(0);
    wr.locals = odin3_u64map_create(0);
    if (wr.unnamed_ids == NULL || wr.locals == NULL) {
        wr.st = ODIN3_ERR_NO_MEMORY;
        wr.building = false;
    }
    odin3_ast_id module = wr.st == ODIN3_OK ? w_module(&wr) : (odin3_ast_id){0};
    odin3_vec_free(&wr.walk);
    odin3_u64map_destroy(wr.unnamed_ids);
    odin3_u64map_destroy(wr.locals);
    odin3_status st = wr.st;
    st = st != ODIN3_OK || e1->stopped ? st : w_replay_done(&wr);
    w_push(&wr, module);
    st = st != ODIN3_OK ? st : wr.st;
    odin3_elab1_env_destroy(e1->env);
    e1->env = NULL;
    return st;
}
