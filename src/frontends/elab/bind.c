/*
 * bind.c — the binder of stage 2 (2D spec §2.5, EL-5; plan Task 13): the 2A symbol table of an
 * elaborated store and its resolution array, with the rows of the EL-4 form (§2.4) that 2A's check
 * rule 7 does not cover.
 *
 * One pre-order sweep (a cursor and an explicit frame stack: no recursion) declares as it goes and
 * queues every reference (IDENT, HIER_NAME, CALL, TASK_CALL, INSTANTIATION) with its scope and
 * every assignment; both queues are worked after the sweep in source order, so a name declared
 * later in its scope resolves (divergence §17 #1). The MODULEs are declared first, from the UNIT's
 * tail, so an INSTANCE is checked against its MODULE's ports wherever that MODULE stands.
 */
#include "frontends/elab/elab_internal.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    BD_SCRATCH_CHUNK = 4096,    /* the scratch arena's chunk */
    BD_SCRATCH_RENEW = 1 << 20, /* scratch bytes after which the arena is replaced */
    BD_INTEGER_BITS = 32,       /* an `integer`: signed [31:0] */
};

/* What a frame's subtree is inside; inherited by the frames below it. */
enum {
    BD_IN_FUNCTION = 1U << 0,
    BD_IN_TASK = 1U << 1,
    BD_IN_INITIAL = 1U << 2,
    BD_IN_BLACKBOX = 1U << 3,
};

/* The role of a LIST frame. */
enum { BD_LIST_OTHER = 0, BD_LIST_MODULE_PORTS, BD_LIST_SUBR_PORTS };

/* What a NUMBER node holds as an integer (bd_integer). */
typedef enum bd_num {
    BD_NUM_OK,         /* a known value within int64 */
    BD_NUM_BIG,        /* a known value outside int64 */
    BD_NUM_XZ,         /* an x or z bit */
    BD_NUM_NOT_NUMBER, /* the node is not a NUMBER */
    BD_NUM_FAILED      /* out of memory (latched) */
} bd_num;

static const char k_blackbox[] = "blackbox";
static const char k_work[] = "work";

/* One node of the sweep's path from the UNIT. */
typedef struct bd_frame {
    odin3_ast_id node;
    odin3_scope_id scope; /* the scope its children are in */
    uint8_t kind;
    uint8_t ctx;  /* BD_IN_* */
    uint8_t role; /* BD_LIST_* */
} bd_frame;

/* A queued reference and the scope it is resolved from. */
typedef struct bd_ref {
    odin3_ast_id node;
    odin3_scope_id scope;
} bd_ref;

/* A queued assignment: the assigning node, its lvalue and its odin3_assign_kind. */
typedef struct bd_assign {
    odin3_ast_id node;
    odin3_ast_id lhs;
    uint8_t kind;
} bd_assign;

/* One component of a HIER_NAME: name, index (ODIN3_SCOPE_NO_INDEX for none) and the length of
 * the path text up to and including it. */
typedef struct bd_part {
    uint32_t name;
    int64_t index;
    size_t text_end;
} bd_part;

/* The binder's run. */
typedef struct bd_run {
    odin3_ast *ast;
    odin3_design *design; /* the store's (diagnostics, strtab, source manager) */
    odin3_symtab *st;
    odin3_elab_binding *out;
    const odin3_elab_limits *limits;
    odin3_arena *scratch;  /* literal values, renewed past BD_SCRATCH_RENEW bytes */
    odin3_vec frames;      /* bd_frame */
    odin3_vec refs;        /* bd_ref */
    odin3_vec assigns;     /* bd_assign */
    odin3_vec work;        /* odin3_ast_id: an lvalue walk */
    odin3_vec dims;        /* odin3_dim: one declarator's unpacked dims */
    odin3_vec parts;       /* bd_part: one HIER_NAME */
    odin3_strbuf text;     /* one HIER_NAME's path */
    odin3_u64map *modules; /* MODULE name -> MODULE node */
    odin3_scope_id design_scope;
    uint32_t work_lib; /* strtab ID of "work" */
    uint32_t errors;
    bool stopped; /* the error cap was reached */
    bool misuse;  /* an EL-4 violation was reported */
    odin3_status fatal;
} bd_run;

/* --- diagnostics ---------------------------------------------------------------------------- */

static void bd_latch(bd_run *br, odin3_status st) {
    if (st != ODIN3_OK && br->fatal == ODIN3_OK) {
        br->fatal = st;
    }
}

static void bd_vemit(bd_run *br, odin3_loc loc, const char *fmt, va_list args) ODIN3_PRINTF(3, 0);

/* One error line at loc (unlocated when 0), in the design's --diag-paths style. */
static void bd_vemit(bd_run *br, odin3_loc loc, const char *fmt, va_list args) {
    if (loc.v != 0) {
        odin3_vdiag(br->design, ODIN3_LOG_ERROR, loc, fmt, args);
        return;
    }
    char text[ODIN3_LOG_BUF];
    if (vsnprintf(text, sizeof text, fmt, args) < 0) {
        text[0] = '\0';
    }
    odin3_log(ODIN3_LOG_ERROR, "%s", text);
}

static void bd_emit(bd_run *br, odin3_loc loc, const char *fmt, ...) ODIN3_PRINTF(3, 4);

static void bd_emit(bd_run *br, odin3_loc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    bd_vemit(br, loc, fmt, args);
    va_end(args);
}

/* True when one more error may be logged (and counts it); at the cap, the stop line instead. */
static bool bd_error_slot(bd_run *br, odin3_loc loc) {
    if (br->stopped) {
        return false;
    }
    if (br->errors < br->limits->max_errors) {
        br->errors++;
        return true;
    }
    br->stopped = true;
    bd_emit(br, loc, "too many errors; elaboration stopped after %" PRIu32, br->limits->max_errors);
    return false;
}

static void bd_error(bd_run *br, odin3_ast_id at, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* A located user error (§9.1) at node at, counted against the cap. */
static void bd_error(bd_run *br, odin3_ast_id at, const char *fmt, ...) {
    odin3_loc loc = odin3_ast_loc(br->ast, at);
    if (!bd_error_slot(br, loc)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    bd_vemit(br, loc, fmt, args);
    va_end(args);
}

static void bd_shape(bd_run *br, odin3_ast_id at, const char *fmt, ...) ODIN3_PRINTF(3, 4);

/* An EL-4 violation at node at: "elaborated form: <rule>" (a misuse of the seam, INVALID_ARG). */
static void bd_shape(bd_run *br, odin3_ast_id at, const char *fmt, ...) {
    br->misuse = true;
    char rule[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    int got = vsnprintf(rule, sizeof rule, fmt, args);
    va_end(args);
    if (got < 0) {
        rule[0] = '\0';
    }
    bd_error(br, at, "elaborated form: %s", rule);
}

static const char *bd_str(const bd_run *br, uint32_t id) {
    const char *str = odin3_strtab_get(odin3_design_strtab(br->design), id);
    return str != NULL ? str : "";
}

static const char *bd_name(const bd_run *br, odin3_ast_id node) {
    return odin3_ast_name_str(br->ast, node);
}

/* M11 at node at: the scope holds first's name already (first located at its declarator). */
static void bd_duplicate(bd_run *br, odin3_ast_id at, odin3_sym_id first) {
    const odin3_symbol *rec = odin3_symbol_get(br->st, first);
    if (rec == NULL) {
        return;
    }
    odin3_ast_id where = rec->declarator.v != 0 ? rec->declarator : rec->decl;
    odin3_srcman *sm = NULL;
    odin3_strbuf pos;
    odin3_strbuf_init(&pos);
    odin3_status st = odin3_design_get_srcman(br->design, &sm);
    st = st != ODIN3_OK ? st
                        : odin3_srcman_format(sm, odin3_ast_loc(br->ast, where), &pos,
                                              odin3_design_diag_paths(br->design));
    bd_latch(br, st);
    if (st == ODIN3_OK) {
        bd_error(br, at, "'%s' is already declared (first at %s)", bd_str(br, rec->name), pos.data);
    }
    odin3_strbuf_free(&pos);
}

/* --- small helpers -------------------------------------------------------------------------- */

static odin3_ast_kind bd_kind(const bd_run *br, odin3_ast_id node) {
    return odin3_ast_kind_of(br->ast, node);
}

static odin3_ast_id bd_child(const bd_run *br, odin3_ast_id node, uint32_t idx) {
    return odin3_ast_child(br->ast, node, idx);
}

/* The children of node after its positional slots (its tail). */
static odin3_ast_span bd_tail(const bd_run *br, odin3_ast_id node) {
    odin3_ast_span kids = odin3_ast_children(br->ast, node);
    uint32_t from = odin3_ast_slot_count(odin3_ast_kind_of(br->ast, node));
    return kids.n > from ? (odin3_ast_span){kids.ids + from, kids.n - from}
                         : (odin3_ast_span){NULL, 0};
}

/* The frame depth levels below the top (0: the top); NULL past the bottom. */
static const bd_frame *bd_frame_at(const bd_run *br, size_t depth) {
    return depth < br->frames.len ? odin3_vec_cat(&br->frames, br->frames.len - 1 - depth) : NULL;
}

static void bd_set_sym(bd_run *br, odin3_ast_id node, odin3_sym_id sym) {
    if (node.v < br->out->n) {
        br->out->sym_of[node.v] = sym;
    }
}

static odin3_sym_id bd_sym(const bd_run *br, odin3_ast_id node) {
    return odin3_elab_sym_of(br->out, node);
}

/* A new scope (and its scope_of entry); 0 after a failure (latched). */
static odin3_scope_id bd_scope(bd_run *br, const odin3_scope_spec *spec) {
    odin3_scope_id scope = {0};
    odin3_status st = odin3_scope_new(br->st, spec, &scope);
    if (st == ODIN3_OK && spec->node.v != 0) {
        st = odin3_u64map_put(br->out->scope_of, (odin3_kv){spec->node.v, scope.v});
    }
    bd_latch(br, st);
    return st == ODIN3_OK ? scope : (odin3_scope_id){0};
}

/* Declares spec's symbol, recorded as node at's (and M11 there when its scope has the name); 0
 * after a duplicate or a failure. */
static odin3_sym_id bd_declare(bd_run *br, const odin3_symbol_spec *spec, odin3_ast_id at) {
    odin3_sym_id old = odin3_symtab_lookup_local(br->st, spec->scope, spec->name);
    if (old.v != 0) {
        bd_duplicate(br, at, old);
        return (odin3_sym_id){0};
    }
    odin3_sym_id sym = {0};
    odin3_status st = odin3_symbol_new(br->st, spec, &sym);
    bd_latch(br, st);
    if (st != ODIN3_OK) {
        return (odin3_sym_id){0};
    }
    bd_set_sym(br, at, sym);
    return sym;
}

/* A symbol found from scope outward within its module (a MODULE symbol of a LIBRARY is never a
 * name of an expression); 0 when none. */
static odin3_sym_id bd_lookup(const bd_run *br, odin3_scope_id scope, uint32_t name) {
    odin3_sym_id sym = odin3_symtab_lookup(br->st, scope, name);
    const odin3_symbol *rec = odin3_symbol_get(br->st, sym);
    const odin3_scope *home = rec != NULL ? odin3_scope_get(br->st, rec->scope) : NULL;
    return home != NULL && home->kind >= ODIN3_SCOPE_MODULE ? sym : (odin3_sym_id){0};
}

static void bd_push_ref(bd_run *br, const bd_frame *fr) {
    bd_ref *slot = odin3_vec_push(&br->refs);
    if (slot == NULL) {
        bd_latch(br, ODIN3_ERR_NO_MEMORY);
        return;
    }
    *slot = (bd_ref){fr->node, fr->scope};
}

static void bd_push_assign(bd_run *br, const bd_assign *assign) {
    bd_assign *slot = odin3_vec_push(&br->assigns);
    if (slot == NULL) {
        bd_latch(br, ODIN3_ERR_NO_MEMORY);
        return;
    }
    *slot = *assign;
}

/* --- constants ------------------------------------------------------------------------------ */

/* The scratch arena, replaced once it holds BD_SCRATCH_RENEW bytes; false out of memory. */
static bool bd_scratch(bd_run *br) {
    if (odin3_arena_bytes_used(br->scratch) < BD_SCRATCH_RENEW) {
        return true;
    }
    odin3_arena *fresh = odin3_arena_create(BD_SCRATCH_CHUNK);
    if (fresh == NULL) {
        bd_latch(br, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    odin3_arena_destroy(br->scratch);
    br->scratch = fresh;
    return true;
}

/* A NUMBER's value (Yosys's sizing, odin3_ast_number_value) as a constant in the scratch arena. */
static bd_num bd_value(bd_run *br, odin3_ast_id node, odin3_cval *out) {
    if (bd_kind(br, node) != ODIN3_AST_NUMBER) {
        return BD_NUM_NOT_NUMBER;
    }
    if (!bd_scratch(br)) {
        return BD_NUM_FAILED;
    }
    odin3_ast_literal lit = {{0}, false, false};
    odin3_status st = odin3_ast_number_value(br->ast, node, br->scratch, &lit);
    st = st != ODIN3_OK ? st : odin3_cval_from_literal(br->scratch, &lit, out);
    bd_latch(br, st);
    if (st != ODIN3_OK) {
        return BD_NUM_FAILED;
    }
    return odin3_cval_is_known(out) ? BD_NUM_OK : BD_NUM_XZ;
}

/* node's value as an integer in *out (BD_NUM_OK) and whether it is negative (OK or BIG). */
static bd_num bd_integer(bd_run *br, odin3_ast_id node, int64_t *out, bool *negative) {
    odin3_cval val = {0};
    bd_num got = bd_value(br, node, &val);
    if (got != BD_NUM_OK) {
        return got;
    }
    *negative =
        (val.flags & ODIN3_CVAL_SIGNED) != 0 && odin3_cval_bit(&val, val.width - 1) == ODIN3_BIT_1;
    return odin3_cval_to_i64(&val, out) ? BD_NUM_OK : BD_NUM_BIG;
}

/* |value| + 1, the width an index of that magnitude implies (EL-46, as stage 1 reports it). */
static uint64_t bd_magnitude_width(int64_t value) {
    uint64_t mag = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    return mag == UINT64_MAX ? mag : mag + 1;
}

static void bd_width_error(bd_run *br, odin3_ast_id at, uint64_t width) {
    bd_error(br, at, "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits", width,
             br->limits->max_width);
}

/* A RANGE bound as an int32 (EL-5: a bound outside int32 is EL-46's width error); false after its
 * error. */
static bool bd_bound(bd_run *br, odin3_ast_id bound, int32_t *out) {
    int64_t value = 0;
    bool negative = false;
    switch (bd_integer(br, bound, &value, &negative)) {
    case BD_NUM_OK:
        if (value >= INT32_MIN && value <= INT32_MAX) {
            *out = (int32_t)value;
            return true;
        }
        bd_width_error(br, bound, bd_magnitude_width(value));
        return false;
    case BD_NUM_BIG:
        bd_width_error(br, bound, UINT64_MAX);
        return false;
    case BD_NUM_XZ:
        bd_error(br, bound, "range bound has x or z bits");
        return false;
    case BD_NUM_NOT_NUMBER:
        bd_shape(br, bound, "a range bound that is not a NUMBER");
        return false;
    default:
        return false;
    }
}

/* A RANGE's bounds; a packed one's width must be within the cap. False after an error. */
static bool bd_range(bd_run *br, odin3_ast_id range, bool packed, odin3_dim *out) {
    int32_t msb = 0;
    int32_t lsb = 0;
    bool msb_ok = bd_bound(br, bd_child(br, range, 0), &msb);
    bool lsb_ok = bd_bound(br, bd_child(br, range, 1), &lsb);
    if (!msb_ok || !lsb_ok) {
        return false;
    }
    int64_t diff = (int64_t)msb - (int64_t)lsb;
    uint64_t span = diff >= 0 ? (uint64_t)diff : (uint64_t)(-diff);
    if (packed && span >= br->limits->max_width) {
        bd_width_error(br, range, span + 1);
        return false;
    }
    *out = (odin3_dim){msb, lsb};
    return true;
}

/* spec's packed dims from range (0: none). */
static void bd_packed(bd_run *br, odin3_ast_id range, odin3_symbol_spec *spec) {
    odin3_dim dim = {0, 0};
    if (range.v != 0 && bd_range(br, range, true, &dim)) {
        spec->has_packed = true;
        spec->msb = dim.msb;
        spec->lsb = dim.lsb;
    }
}

/* An `integer` without a range: signed [31:0]. */
static void bd_integer_type(odin3_symbol_spec *spec) {
    if (!spec->has_packed) {
        spec->has_packed = true;
        spec->msb = BD_INTEGER_BITS - 1;
        spec->lsb = 0;
    }
    spec->flags = (uint16_t)(spec->flags | ODIN3_SYM_F_SIGNED);
}

/* The unpacked dims of declarator (its RANGEs, outermost first) on sym. */
static void bd_unpacked(bd_run *br, odin3_ast_id declarator, odin3_sym_id sym) {
    odin3_ast_span ranges = bd_tail(br, declarator);
    odin3_vec_clear(&br->dims);
    for (uint32_t i = 0; i < ranges.n; i++) {
        odin3_dim dim = {0, 0};
        if (!bd_range(br, ranges.ids[i], false, &dim)) {
            continue;
        }
        odin3_dim *slot = odin3_vec_push(&br->dims);
        if (slot == NULL) {
            bd_latch(br, ODIN3_ERR_NO_MEMORY);
            return;
        }
        *slot = dim;
    }
    if (br->dims.len > 0 && sym.v != 0) {
        bd_latch(br, odin3_symbol_set_unpacked(br->st, sym, br->dims.data, (uint32_t)br->dims.len));
    }
}

/* A localparam's value (a NUMBER's bits as Yosys sizes them, or a STRING's) on sym. */
static void bd_param_value(bd_run *br, odin3_sym_id sym, odin3_ast_id value) {
    if (!bd_scratch(br)) {
        return;
    }
    odin3_value val = {0};
    bool is_signed = false;
    odin3_status st = ODIN3_OK;
    if (bd_kind(br, value) == ODIN3_AST_NUMBER) {
        odin3_ast_literal lit = {{0}, false, false};
        st = odin3_ast_number_value(br->ast, value, br->scratch, &lit);
        val = lit.value;
        is_signed = lit.is_signed;
    } else {
        odin3_cval text = {0};
        st = odin3_cval_from_string(br->scratch, odin3_bytes_cstr(bd_name(br, value)), &text);
        st = st != ODIN3_OK ? st : odin3_cval_to_value(br->scratch, &text, &val);
    }
    bd_latch(br, st != ODIN3_OK ? st : odin3_symbol_set_value(br->st, sym, &val, is_signed));
}

/* --- declarations --------------------------------------------------------------------------- */

/* A net kind the elaborated form excludes (EL-4: tri0, tri1, trireg) on declarator, named. */
static void bd_net_kind(bd_run *br, odin3_ast_id declarator, uint32_t kind) {
    if (kind == ODIN3_AST_NET_DECL_TRI0 || kind == ODIN3_AST_NET_DECL_TRI1 ||
        kind == ODIN3_AST_NET_DECL_TRIREG) {
        const odin3_ast_kind_info *info = odin3_ast_kind_info_of(ODIN3_AST_NET_DECL);
        bd_shape(br, declarator, "net '%s' of kind '%s'", bd_name(br, declarator),
                 info->sub_names[kind]);
    }
}

/* A NET_DECL has no `vectored` or `scalared` (EL-4). */
static void bd_net_decl(bd_run *br, odin3_ast_id node) {
    uint16_t flags = odin3_ast_flags(br->ast, node);
    if ((flags & ODIN3_AST_F_NET_DECL_VECTORED) != 0) {
        bd_shape(br, node, "'vectored' on a net declaration");
    }
    if ((flags & ODIN3_AST_F_NET_DECL_SCALARED) != 0) {
        bd_shape(br, node, "'scalared' on a net declaration");
    }
}

/* A GATE_DECL is a logic gate, not a switch-level or pull one (EL-4). */
static void bd_gate(bd_run *br, odin3_ast_id node) {
    if (odin3_ast_sub(br->ast, node) >= ODIN3_AST_GATE_DECL_NMOS) {
        bd_shape(br, node, "switch-level or pull gate '%s'", odin3_ast_sub_name(br->ast, node));
    }
}

/* A port's symbol: a module port is a net (its NET field) or a reg/integer variable (its DT); a
 * function or task port is a variable. */
static void bd_port(bd_run *br, const bd_frame *decl, odin3_ast_id declarator, bool subroutine) {
    uint32_t flags = odin3_ast_flags(br->ast, decl->node);
    uint32_t dt = (flags & (uint32_t)ODIN3_AST_F_PORT_DECL_DT_MASK) >>
                  (uint32_t)ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    uint32_t net = (flags & (uint32_t)ODIN3_AST_F_PORT_DECL_NET_MASK) >>
                   (uint32_t)ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    bool is_var =
        subroutine || (net == 0 && (dt == ODIN3_AST_DT_REG || dt == ODIN3_AST_DT_INTEGER));
    if (!subroutine && !is_var && (net == 0 || dt != ODIN3_AST_DT_NONE)) {
        bd_shape(br, declarator, "port '%s' without a net kind", bd_name(br, declarator));
        net = 1U + (uint32_t)ODIN3_AST_NET_DECL_WIRE; /* declared all the same: no cascade */
    }
    if (!is_var) {
        bd_net_kind(br, declarator, net - 1U);
    }
    uint32_t var_sub =
        dt == ODIN3_AST_DT_INTEGER ? ODIN3_AST_VAR_DECL_INTEGER : ODIN3_AST_VAR_DECL_REG;
    odin3_symbol_spec spec = {
        .name = odin3_ast_name(br->ast, declarator),
        .scope = decl->scope,
        .kind = is_var ? ODIN3_SYM_VAR : ODIN3_SYM_NET,
        .sub = (uint8_t)(is_var ? var_sub : net - 1U),
        .flags = (uint16_t)(ODIN3_SYM_F_PORT |
                            ((flags & ODIN3_AST_F_PORT_DECL_SIGNED) != 0 ? ODIN3_SYM_F_SIGNED : 0)),
        .dir = (uint8_t)(1U + odin3_ast_sub(br->ast, decl->node)),
        .decl = decl->node,
        .declarator = declarator,
    };
    bd_packed(br, bd_child(br, decl->node, 0), &spec);
    if (is_var && dt == ODIN3_AST_DT_INTEGER) {
        bd_integer_type(&spec);
    }
    bd_unpacked(br, declarator, bd_declare(br, &spec, declarator));
}

_Static_assert(ODIN3_AST_F_NET_DECL_SIGNED == ODIN3_AST_F_VAR_DECL_SIGNED,
               "NET_DECL and VAR_DECL share the SIGNED bit");

/* A NET_DECL's or VAR_DECL's declarator; a net declaration assignment is queued (CONTINUOUS). */
static void bd_object(bd_run *br, const bd_frame *decl, odin3_ast_id declarator) {
    bool is_net = decl->kind == ODIN3_AST_NET_DECL;
    uint32_t sub = odin3_ast_sub(br->ast, decl->node);
    if (is_net) {
        bd_net_kind(br, declarator, sub);
    }
    odin3_symbol_spec spec = {
        .name = odin3_ast_name(br->ast, declarator),
        .scope = decl->scope,
        .kind = is_net ? ODIN3_SYM_NET : ODIN3_SYM_VAR,
        .sub = (uint8_t)sub,
        .flags =
            (uint16_t)((odin3_ast_flags(br->ast, decl->node) & ODIN3_AST_F_NET_DECL_SIGNED) != 0
                           ? ODIN3_SYM_F_SIGNED
                           : 0),
        .decl = decl->node,
        .declarator = declarator,
    };
    bd_packed(br, bd_child(br, decl->node, is_net ? 1 : 0), &spec);
    if (!is_net && sub == ODIN3_AST_VAR_DECL_INTEGER) {
        bd_integer_type(&spec);
    }
    bd_unpacked(br, declarator, bd_declare(br, &spec, declarator));
    if (is_net && bd_child(br, declarator, 0).v != 0) {
        bd_assign net_assign = {declarator, declarator, ODIN3_ASSIGN_CONTINUOUS};
        bd_push_assign(br, &net_assign);
    }
}

/* A PARAM_DECL's declarator: a localparam with a NUMBER or STRING value (EL-4), whose symbol
 * carries the value; a black box's parameters are checked only (its scope holds its ports). */
static void bd_param(bd_run *br, const bd_frame *decl, odin3_ast_id declarator) {
    odin3_ast_id value = bd_child(br, declarator, 0);
    odin3_ast_kind kind = bd_kind(br, value);
    if (odin3_ast_sub(br->ast, decl->node) != ODIN3_AST_PARAM_DECL_LOCALPARAM ||
        (kind != ODIN3_AST_NUMBER && kind != ODIN3_AST_STRING)) {
        bd_shape(br, declarator, "parameter '%s' is not a localparam with a NUMBER or STRING value",
                 bd_name(br, declarator));
        return;
    }
    if ((decl->ctx & BD_IN_BLACKBOX) != 0) {
        return;
    }
    bool is_signed = kind == ODIN3_AST_NUMBER &&
                     (odin3_ast_flags(br->ast, value) & ODIN3_AST_F_NUMBER_SIGNED) != 0;
    odin3_symbol_spec spec = {
        .name = odin3_ast_name(br->ast, declarator),
        .scope = decl->scope,
        .kind = ODIN3_SYM_LOCALPARAM,
        .flags = (uint16_t)(is_signed ? ODIN3_SYM_F_SIGNED : 0),
        .decl = decl->node,
        .declarator = declarator,
    };
    bd_packed(br, bd_child(br, decl->node, 0), &spec);
    odin3_sym_id sym = bd_declare(br, &spec, declarator);
    if (sym.v != 0) {
        bd_param_value(br, sym, value);
    }
}

static void bd_declarator(bd_run *br, const bd_frame *up, odin3_ast_id node) {
    switch (up->kind) {
    case ODIN3_AST_PORT_DECL: {
        const bd_frame *list = bd_frame_at(br, 1);
        bd_port(br, up, node, list != NULL && list->role == BD_LIST_SUBR_PORTS);
        break;
    }
    case ODIN3_AST_NET_DECL:
    case ODIN3_AST_VAR_DECL:
        bd_object(br, up, node);
        break;
    case ODIN3_AST_PARAM_DECL:
        bd_param(br, up, node);
        break;
    default:
        break;
    }
}

/* The library of a MODULE: its FILE buffer's (2A §3.1), "work" when the buffer has none. Out of
 * memory creating the source manager latches. */
static uint32_t bd_library(bd_run *br, odin3_ast_id module) {
    odin3_srcman *sm = NULL;
    odin3_srcbuf_info info = {0};
    odin3_status st = odin3_design_get_srcman(br->design, &sm);
    if (st != ODIN3_OK) {
        bd_latch(br, st);
        return br->work_lib;
    }
    odin3_loc at = odin3_srcman_file_loc(sm, odin3_ast_loc(br->ast, module));
    if (!odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, at), &info) || info.library == 0) {
        return br->work_lib;
    }
    return info.library;
}

static odin3_scope_id bd_library_scope(bd_run *br, uint32_t library) {
    odin3_scope_id scope =
        odin3_scope_child(br->st, br->design_scope, library, ODIN3_SCOPE_NO_INDEX);
    if (scope.v != 0) {
        return scope;
    }
    odin3_scope_spec spec = {
        .kind = ODIN3_SCOPE_LIBRARY, .name = library, .parent = br->design_scope};
    return bd_scope(br, &spec);
}

/* A MODULE of the UNIT's tail: its LIBRARY scope (made once), MODULE symbol and MODULE scope; M11
 * for a second MODULE of one name, in any library (names are unique in the UNIT, point (q)). */
static void bd_module(bd_run *br, odin3_ast_id module) {
    uint32_t name = odin3_ast_name(br->ast, module);
    uint64_t first = 0;
    if (odin3_u64map_get(br->modules, name, &first)) {
        bd_duplicate(br, module, bd_sym(br, (odin3_ast_id){(uint32_t)first}));
        return;
    }
    odin3_scope_id library = bd_library_scope(br, bd_library(br, module));
    if (library.v == 0) {
        return;
    }
    odin3_symbol_spec sym = {
        .name = name, .scope = library, .kind = ODIN3_SYM_MODULE, .decl = module};
    if (bd_declare(br, &sym, module).v == 0) {
        return;
    }
    bd_latch(br, odin3_u64map_put(br->modules, (odin3_kv){name, module.v}));
    odin3_scope_spec spec = {
        .kind = ODIN3_SCOPE_MODULE, .name = name, .parent = library, .node = module};
    (void)bd_scope(br, &spec);
}

static void bd_modules(bd_run *br, odin3_ast_id unit) {
    odin3_ast_span mods = odin3_ast_children(br->ast, unit);
    for (uint32_t i = 0; i < mods.n && br->fatal == ODIN3_OK && !br->stopped; i++) {
        if (bd_kind(br, mods.ids[i]) == ODIN3_AST_MODULE) {
            bd_module(br, mods.ids[i]);
        }
    }
}

/* --- scopes of the sweep -------------------------------------------------------------------- */

static bool bd_is_blackbox(const bd_run *br, odin3_ast_id module) {
    uint32_t id = 0;
    if (!odin3_strtab_find(odin3_design_strtab(br->design), odin3_bytes_cstr(k_blackbox), &id)) {
        return false;
    }
    odin3_ast_span attrs = odin3_ast_attrs(br->ast, module);
    for (uint32_t i = 0; i < attrs.n; i++) {
        if (odin3_ast_name(br->ast, attrs.ids[i]) == id) {
            return true;
        }
    }
    return false;
}

/* A MODULE frame: the scope declared for it (none for a duplicate: its subtree is skipped). */
static bool bd_enter_module(const bd_run *br, bd_frame *fr) {
    fr->scope = odin3_elab_scope_of(br->out, fr->node);
    fr->ctx = bd_is_blackbox(br, fr->node) ? BD_IN_BLACKBOX : 0;
    return fr->scope.v == 0;
}

static void bd_list_role(const bd_run *br, const bd_frame *up, bd_frame *fr) {
    if (up->kind == ODIN3_AST_MODULE && bd_child(br, up->node, 1).v == fr->node.v) {
        fr->role = BD_LIST_MODULE_PORTS;
    } else if ((up->kind == ODIN3_AST_FUNCTION_DECL && bd_child(br, up->node, 1).v == fr->node.v) ||
               (up->kind == ODIN3_AST_TASK_DECL && bd_child(br, up->node, 0).v == fr->node.v)) {
        fr->role = BD_LIST_SUBR_PORTS;
    }
}

/* A generate-block index or a HIER_NAME index: a 32-bit NUMBER; false after the violation. */
static bool bd_index(bd_run *br, odin3_ast_id node, int32_t *out) {
    int64_t value = 0;
    bool negative = false;
    bd_num got = bd_integer(br, node, &value, &negative);
    if (got == BD_NUM_OK && value >= INT32_MIN && value <= INT32_MAX) {
        *out = (int32_t)value;
        return true;
    }
    if (got != BD_NUM_FAILED) {
        bd_shape(br, node, "a generate index that is not a 32-bit NUMBER");
    }
    return false;
}

/* The BLOCK symbol of a GEN_BLOCK or named SEQ_BLOCK whose scope spec describes; a further
 * iteration of a loop's block shares the first one's. False after M11 (or a failure). */
static bool bd_block_symbol(bd_run *br, const odin3_scope_spec *spec, odin3_ast_id node) {
    odin3_sym_id old = odin3_symtab_lookup_local(br->st, spec->parent, spec->name);
    const odin3_symbol *rec = odin3_symbol_get(br->st, old);
    if (rec != NULL) {
        bool iteration = rec->kind == ODIN3_SYM_BLOCK && spec->has_index &&
                         bd_kind(br, rec->decl) == ODIN3_AST_GEN_BLOCK &&
                         bd_child(br, rec->decl, 0).v != 0 &&
                         odin3_scope_child(br->st, spec->parent, spec->name, spec->index).v == 0;
        if (!iteration) {
            bd_duplicate(br, node, old);
            return false;
        }
        bd_set_sym(br, node, old);
        return true;
    }
    odin3_symbol_spec sym = {
        .name = spec->name, .scope = spec->parent, .kind = ODIN3_SYM_BLOCK, .decl = node};
    return bd_declare(br, &sym, node).v != 0;
}

/* A GEN_BLOCK: its GENERATE scope (index from its slot) and BLOCK symbol. True: skip it. */
static bool bd_gen_block(bd_run *br, bd_frame *fr) {
    odin3_scope_spec spec = {.kind = ODIN3_SCOPE_GENERATE,
                             .name = odin3_ast_name(br->ast, fr->node),
                             .parent = fr->scope,
                             .node = fr->node};
    odin3_ast_id slot = bd_child(br, fr->node, 0);
    if (slot.v != 0 && !bd_index(br, slot, &spec.index)) {
        return true;
    }
    spec.has_index = slot.v != 0;
    if (spec.name != 0 && !bd_block_symbol(br, &spec, fr->node)) {
        return true;
    }
    fr->scope = bd_scope(br, &spec);
    return fr->scope.v == 0;
}

/* The first DECLARATOR of the declarations in a locals LIST (0: none). */
static odin3_ast_id bd_first_local(const bd_run *br, odin3_ast_id list) {
    odin3_ast_span decls = odin3_ast_children(br->ast, list);
    for (uint32_t i = 0; list.v != 0 && i < decls.n; i++) {
        odin3_ast_span declarators = bd_tail(br, decls.ids[i]);
        if (declarators.n > 0) {
            return declarators.ids[0];
        }
    }
    return (odin3_ast_id){0};
}

/* A SEQ_BLOCK: a named one is a BLOCK scope and symbol; an unnamed one has no declarations
 * (EL-4). True: skip it. */
static bool bd_seq_block(bd_run *br, bd_frame *fr) {
    uint32_t name = odin3_ast_name(br->ast, fr->node);
    if (name == 0) {
        odin3_ast_id first = bd_first_local(br, bd_child(br, fr->node, 0));
        if (first.v != 0) {
            bd_shape(br, fr->node, "unnamed block '%s' with declarations", bd_name(br, first));
        }
        return first.v != 0;
    }
    odin3_scope_spec spec = {
        .kind = ODIN3_SCOPE_BLOCK, .name = name, .parent = fr->scope, .node = fr->node};
    if (!bd_block_symbol(br, &spec, fr->node)) {
        return true;
    }
    fr->scope = bd_scope(br, &spec);
    return fr->scope.v == 0;
}

/* A function's result: the VAR named like it in its scope, of its declared type. */
static void bd_result(bd_run *br, const bd_frame *fr) {
    uint32_t flags = odin3_ast_flags(br->ast, fr->node);
    uint32_t dt = (flags & (uint32_t)ODIN3_AST_F_FUNCTION_DECL_DT_MASK) >>
                  (uint32_t)ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT;
    odin3_symbol_spec spec = {
        .name = odin3_ast_name(br->ast, fr->node),
        .scope = fr->scope,
        .kind = ODIN3_SYM_VAR,
        .sub = (uint8_t)(dt == ODIN3_AST_DT_INTEGER ? ODIN3_AST_VAR_DECL_INTEGER
                                                    : ODIN3_AST_VAR_DECL_REG),
        .flags =
            (uint16_t)((flags & ODIN3_AST_F_FUNCTION_DECL_SIGNED) != 0 ? ODIN3_SYM_F_SIGNED : 0),
        .decl = fr->node,
    };
    bd_packed(br, bd_child(br, fr->node, 0), &spec);
    if (dt == ODIN3_AST_DT_INTEGER) {
        bd_integer_type(&spec);
    }
    odin3_sym_id sym = {0};
    bd_latch(br, odin3_symbol_new(br->st, &spec, &sym));
}

/* A FUNCTION_DECL or TASK_DECL: its symbol, its scope and (a function) its result. True: skip. */
static bool bd_subroutine(bd_run *br, bd_frame *fr) {
    bool is_function = fr->kind == ODIN3_AST_FUNCTION_DECL;
    uint32_t name = odin3_ast_name(br->ast, fr->node);
    odin3_symbol_spec sym = {.name = name,
                             .scope = fr->scope,
                             .kind = is_function ? ODIN3_SYM_FUNCTION : ODIN3_SYM_TASK,
                             .decl = fr->node};
    if (bd_declare(br, &sym, fr->node).v == 0) {
        return true;
    }
    odin3_scope_spec spec = {.kind = is_function ? ODIN3_SCOPE_FUNCTION : ODIN3_SCOPE_TASK,
                             .name = name,
                             .parent = fr->scope,
                             .node = fr->node};
    fr->scope = bd_scope(br, &spec);
    fr->ctx = (uint8_t)(fr->ctx | (is_function ? BD_IN_FUNCTION : BD_IN_TASK));
    if (fr->scope.v != 0 && is_function) {
        bd_result(br, fr);
    }
    return fr->scope.v == 0;
}

/* The name of a port PORT_DECL's (first) declarator; 0 for anything else. */
static uint32_t bd_port_name(const bd_run *br, odin3_ast_id port) {
    odin3_ast_span decls = bd_tail(br, port);
    return bd_kind(br, port) == ODIN3_AST_PORT_DECL && decls.n > 0
               ? odin3_ast_name(br->ast, decls.ids[0])
               : 0;
}

/* An INSTANCE: its symbol (a named one) and, for a MODULE of the store, one named CONNECTION per
 * port of that MODULE in port order (EL-4). */
static void bd_instance(bd_run *br, const bd_frame *up, odin3_ast_id node) {
    if (bd_child(br, node, 0).v != 0) { /* arrays are expanded (EL-4) */
        bd_shape(br, node, "instance '%s' with a range", bd_name(br, node));
    }
    if (odin3_ast_name(br->ast, node) != 0) {
        odin3_symbol_spec spec = {.name = odin3_ast_name(br->ast, node),
                                  .scope = up->scope,
                                  .kind = ODIN3_SYM_INSTANCE,
                                  .decl = up->node,
                                  .declarator = node};
        (void)bd_declare(br, &spec, node);
    }
    uint64_t module = 0;
    if (up->kind != ODIN3_AST_INSTANTIATION ||
        !odin3_u64map_get(br->modules, odin3_ast_name(br->ast, up->node), &module)) {
        return;
    }
    odin3_ast_id ports = bd_child(br, (odin3_ast_id){(uint32_t)module}, 1);
    odin3_ast_span conns = bd_tail(br, node);
    uint32_t n_ports = ports.v != 0 ? odin3_ast_nchild(br->ast, ports) : 0;
    bool ok = conns.n == n_ports;
    for (uint32_t i = 0; ok && i < n_ports; i++) {
        uint32_t port = bd_port_name(br, bd_child(br, ports, i));
        ok = port != 0 && bd_kind(br, conns.ids[i]) == ODIN3_AST_CONNECTION &&
             odin3_ast_name(br->ast, conns.ids[i]) == port;
    }
    if (!ok) {
        bd_shape(br, node, "instance '%s' does not connect every port of '%s' once, in port order",
                 bd_name(br, node), bd_name(br, up->node));
    }
}

/* --- statements and expressions ------------------------------------------------------------- */

/* A TIMING_STMT is only an ALWAYS's statement, whose event list is all edges or all levels. */
static void bd_timing(bd_run *br, const bd_frame *up, odin3_ast_id node) {
    if (up->kind != ODIN3_AST_ALWAYS) {
        bd_shape(br, node, "'TIMING_STMT' outside an always");
        return;
    }
    odin3_ast_id control = bd_child(br, node, 0);
    if (bd_kind(br, control) != ODIN3_AST_EVENT_CONTROL) {
        return; /* rule 7's */
    }
    odin3_ast_span events = bd_tail(br, control);
    bool edge = false;
    bool level = false;
    for (uint32_t i = 0; i < events.n; i++) {
        bool is_level = odin3_ast_sub(br->ast, events.ids[i]) == ODIN3_AST_EVENT_EXPR_NONE;
        level = level || is_level;
        edge = edge || !is_level;
    }
    if (edge && level) {
        bd_shape(br, control, "edge and level events in one event list");
    }
}

/* A procedural assignment: no intra-assignment control (EL-4); its lvalue is queued outside
 * function and task bodies. */
static void bd_proc_assign(bd_run *br, const bd_frame *fr) {
    odin3_ast_id control = bd_child(br, fr->node, 1);
    if (control.v != 0) {
        bd_shape(br, control, "intra-assignment control");
    }
    if ((fr->ctx & (BD_IN_FUNCTION | BD_IN_TASK)) != 0) {
        return;
    }
    bd_assign assign = {fr->node, bd_child(br, fr->node, 0),
                        fr->kind == ODIN3_AST_BLOCKING_ASSIGN ? ODIN3_ASSIGN_BLOCKING
                                                              : ODIN3_ASSIGN_NONBLOCKING};
    bd_push_assign(br, &assign);
}

/* A REPEAT or REPLICATE count: a NUMBER of 0 or more (EL-4). */
static void bd_count(bd_run *br, odin3_ast_id count) {
    int64_t value = 0;
    bool negative = false;
    bd_num got = bd_integer(br, count, &value, &negative);
    if (got == BD_NUM_NOT_NUMBER || got == BD_NUM_XZ ||
        ((got == BD_NUM_OK || got == BD_NUM_BIG) && negative)) {
        bd_shape(br, count, "a count that is not a NUMBER of 0 or more");
    }
}

/* A part select's bounds and an indexed part select's width are NUMBERs (EL-4); the object may be
 * any expression (an instance-array slice, read at its self-determined width, v4.8). */
static void bd_select_bound(bd_run *br, odin3_ast_id node, uint32_t slot) {
    odin3_ast_id bound = bd_child(br, node, slot);
    if (bd_kind(br, bound) != ODIN3_AST_NUMBER) {
        bd_shape(br, bound.v != 0 ? bound : node, "a select bound or width that is not a NUMBER");
    }
}

static void bd_select(bd_run *br, odin3_ast_id node) {
    uint32_t sub = odin3_ast_sub(br->ast, node);
    if (sub == ODIN3_AST_SELECT_BIT) {
        return;
    }
    if (sub == ODIN3_AST_SELECT_PART) {
        bd_select_bound(br, node, 1);
    }
    bd_select_bound(br, node, 2);
}

/* A CALL: a system one is $signed or $unsigned (EL-4); a user one is resolved later. */
static void bd_call(bd_run *br, const bd_frame *fr) {
    uint32_t flags = odin3_ast_flags(br->ast, fr->node);
    const char *name = bd_name(br, fr->node);
    if ((flags & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        if (strcmp(name, "$signed") != 0 && strcmp(name, "$unsigned") != 0) {
            bd_shape(br, fr->node, "system function '%s' (only $signed and $unsigned are allowed)",
                     name);
        }
        return;
    }
    if ((flags & ODIN3_AST_F_CALL_HIER) != 0) {
        bd_error(br, fr->node, "hierarchical reference '%s' is not supported", name);
        return;
    }
    bd_push_ref(br, fr);
}

/* A TASK_CALL: a system one is $readmemh or $readmemb inside an INITIAL (EL-4, S4); a user one is
 * resolved later. */
static void bd_task_call(bd_run *br, const bd_frame *fr) {
    uint32_t flags = odin3_ast_flags(br->ast, fr->node);
    const char *name = bd_name(br, fr->node);
    if ((flags & ODIN3_AST_F_TASK_CALL_SYSTEM) != 0) {
        if (strcmp(name, "$readmemh") != 0 && strcmp(name, "$readmemb") != 0) {
            bd_shape(br, fr->node, "system task '%s' (only $readmemh and $readmemb are allowed)",
                     name);
        } else if ((fr->ctx & BD_IN_INITIAL) == 0) {
            bd_shape(br, fr->node, "system task '%s' outside an initial block", name);
        }
        return;
    }
    if ((flags & ODIN3_AST_F_TASK_CALL_HIER) != 0) {
        bd_error(br, fr->node, "hierarchical reference '%s' is not supported", name);
        return;
    }
    bd_push_ref(br, fr);
}

/* Statements and expressions; true: skip the node's children. */
static bool bd_visit_expr(bd_run *br, const bd_frame *up, const bd_frame *fr) {
    switch (fr->kind) {
    case ODIN3_AST_NET_ASSIGN: {
        bd_assign assign = {fr->node, bd_child(br, fr->node, 0), ODIN3_ASSIGN_CONTINUOUS};
        bd_push_assign(br, &assign);
        return false;
    }
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        bd_proc_assign(br, fr);
        return false;
    case ODIN3_AST_TIMING_STMT:
        bd_timing(br, up, fr->node);
        return false;
    case ODIN3_AST_WHILE:
        if ((fr->ctx & BD_IN_FUNCTION) == 0) {
            bd_shape(br, fr->node, "'WHILE' outside a function");
        }
        return false;
    case ODIN3_AST_REPEAT:
    case ODIN3_AST_REPLICATE:
        bd_count(br, bd_child(br, fr->node, 0));
        return false;
    case ODIN3_AST_SELECT:
        bd_select(br, fr->node);
        return false;
    case ODIN3_AST_CALL:
        bd_call(br, fr);
        return false;
    case ODIN3_AST_TASK_CALL:
        bd_task_call(br, fr);
        return false;
    case ODIN3_AST_IDENT:
    case ODIN3_AST_INSTANTIATION:
        bd_push_ref(br, fr);
        return false;
    case ODIN3_AST_HIER_NAME:
        bd_push_ref(br, fr);
        return true; /* its components are resolved with it */
    default:
        return false;
    }
}

/* Declarations and instances; true: skip the node's children. */
static bool bd_visit_decl(bd_run *br, const bd_frame *up, const bd_frame *fr) {
    switch (fr->kind) {
    case ODIN3_AST_DECLARATOR:
        bd_declarator(br, up, fr->node);
        return false;
    case ODIN3_AST_RANGE:
        return true; /* constant bounds, read by the declaration that owns it */
    case ODIN3_AST_NET_DECL:
        bd_net_decl(br, fr->node);
        return false;
    case ODIN3_AST_GATE_DECL:
        bd_gate(br, fr->node);
        return false;
    case ODIN3_AST_INSTANCE:
        bd_instance(br, up, fr->node);
        return false;
    default:
        return bd_visit_expr(br, up, fr);
    }
}

/* The node of fr, entered under up (NULL for the UNIT); true: skip its children. */
static bool bd_visit(bd_run *br, const bd_frame *up, bd_frame *fr) {
    if (up == NULL) {
        return false;
    }
    if ((up->ctx & BD_IN_BLACKBOX) != 0 && up->kind == ODIN3_AST_MODULE &&
        fr->kind != ODIN3_AST_LIST) {
        return true; /* a black box has no items (EL-4); its scope holds its ports only */
    }
    switch (fr->kind) {
    case ODIN3_AST_MODULE:
        return bd_enter_module(br, fr);
    case ODIN3_AST_LIST:
        bd_list_role(br, up, fr);
        return false;
    case ODIN3_AST_GEN_BLOCK:
        return bd_gen_block(br, fr);
    case ODIN3_AST_SEQ_BLOCK:
        return bd_seq_block(br, fr);
    case ODIN3_AST_FUNCTION_DECL:
    case ODIN3_AST_TASK_DECL:
        return bd_subroutine(br, fr);
    case ODIN3_AST_INITIAL:
        fr->ctx = (uint8_t)(fr->ctx | BD_IN_INITIAL);
        return false;
    default:
        return bd_visit_decl(br, up, fr);
    }
}

/* The ENTER of node: its frame, made from its parent's, visited and pushed. */
static void bd_enter(bd_run *br, odin3_ast_cursor *cur, odin3_ast_id node) {
    const bd_frame *up = bd_frame_at(br, 0);
    bd_frame fr = {node, up != NULL ? up->scope : br->design_scope,
                   (uint8_t)odin3_ast_kind_of(br->ast, node), up != NULL ? up->ctx : 0,
                   BD_LIST_OTHER};
    bool skip = bd_visit(br, up, &fr);
    bd_frame *slot = odin3_vec_push(&br->frames);
    if (slot == NULL) {
        bd_latch(br, ODIN3_ERR_NO_MEMORY);
        return;
    }
    *slot = fr;
    if (skip) {
        odin3_ast_cursor_skip(cur);
    }
}

static void bd_sweep(bd_run *br, odin3_ast_id unit) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, br->ast, unit);
    bd_latch(br, st);
    odin3_ast_event ev = ODIN3_AST_ENTER;
    odin3_ast_id node = {0};
    while (br->fatal == ODIN3_OK && !br->stopped && odin3_ast_cursor_next(&cur, &ev, &node)) {
        if (ev == ODIN3_AST_LEAVE) {
            odin3_vec_pop(&br->frames);
        } else {
            bd_enter(br, &cur, node);
        }
    }
    odin3_ast_cursor_free(&cur);
}

/* --- resolution ----------------------------------------------------------------------------- */

/* A name of an expression (IDENT or HIER_NAME node, text its spelling) resolved to sym: a net,
 * variable or parameter; an instance, function or task is an error (Yosys: "Failed to detect width
 * for identifier"). A block's name binds to its BLOCK symbol (Task 18's carry: Yosys makes it an
 * implicit net). */
static void bd_value_name(bd_run *br, odin3_ast_id node, odin3_sym_id sym, const char *text) {
    const odin3_symbol *rec = odin3_symbol_get(br->st, sym);
    if (rec == NULL) {
        bd_error(br, node, "'%s' is not declared", text);
    } else if (rec->kind == ODIN3_SYM_INSTANCE || rec->kind == ODIN3_SYM_FUNCTION ||
               rec->kind == ODIN3_SYM_TASK) {
        bd_error(br, node, "'%s' is not a net, variable or parameter", text);
    } else {
        bd_set_sym(br, node, sym);
    }
}

static void bd_resolve_ident(bd_run *br, const bd_ref *ref) {
    odin3_sym_id sym = bd_lookup(br, ref->scope, odin3_ast_name(br->ast, ref->node));
    bd_value_name(br, ref->node, sym, bd_name(br, ref->node));
}

/* A CALL names a FUNCTION (its own result variable aside), a TASK_CALL a TASK. */
static void bd_resolve_call(bd_run *br, const bd_ref *ref) {
    bool is_call = bd_kind(br, ref->node) == ODIN3_AST_CALL;
    uint32_t name = odin3_ast_name(br->ast, ref->node);
    odin3_sym_id sym = bd_lookup(br, ref->scope, name);
    const odin3_symbol *rec = odin3_symbol_get(br->st, sym);
    if (is_call && rec != NULL && rec->kind == ODIN3_SYM_VAR &&
        bd_kind(br, rec->decl) == ODIN3_AST_FUNCTION_DECL) {
        sym = bd_lookup(br, odin3_scope_get(br->st, rec->scope)->parent, name);
        rec = odin3_symbol_get(br->st, sym);
    }
    uint8_t want = is_call ? ODIN3_SYM_FUNCTION : ODIN3_SYM_TASK;
    if (rec == NULL) {
        bd_error(br, ref->node, "'%s' is not declared", bd_str(br, name));
    } else if (rec->kind != want) {
        bd_error(br, ref->node, is_call ? "'%s' is not a function" : "'%s' is not a task",
                 bd_str(br, name));
    } else {
        bd_set_sym(br, ref->node, sym);
    }
}

/* An INSTANTIATION names a MODULE of the store (its MODULE symbol) or a registered cell type. */
static void bd_resolve_inst(bd_run *br, odin3_ast_id node) {
    uint64_t module = 0;
    if (odin3_u64map_get(br->modules, odin3_ast_name(br->ast, node), &module)) {
        bd_set_sym(br, node, bd_sym(br, (odin3_ast_id){(uint32_t)module}));
    }
}

/* The text of one HIER_NAME component: name or name[index] ("?" for a malformed one). */
static odin3_status bd_part_text(bd_run *br, const bd_part *rec, odin3_ast_id ident) {
    const char *dot = br->text.len > 0 ? "." : "";
    if (rec->name == 0) {
        return odin3_strbuf_appendf(&br->text, "%s?", dot);
    }
    if (rec->index == ODIN3_SCOPE_NO_INDEX) {
        return odin3_strbuf_appendf(&br->text, "%s%s", dot, bd_name(br, ident));
    }
    return odin3_strbuf_appendf(&br->text, "%s%s[%" PRId64 "]", dot, bd_name(br, ident),
                                rec->index);
}

/* One HIER_NAME component (an IDENT, or a bit SELECT of an IDENT by a NUMBER) into br->parts and
 * br->text; *bad for a malformed one. False after an index violation or out of memory. */
static bool bd_hier_part(bd_run *br, odin3_ast_id part, bool *bad) {
    bd_part rec = {0, ODIN3_SCOPE_NO_INDEX, 0};
    odin3_ast_id ident = part;
    bool ok = true;
    if (bd_kind(br, part) == ODIN3_AST_SELECT) {
        ident = bd_child(br, part, 0);
        ok = odin3_ast_sub(br->ast, part) == ODIN3_AST_SELECT_BIT &&
             bd_kind(br, ident) == ODIN3_AST_IDENT;
        int32_t index = 0;
        if (ok && !bd_index(br, bd_child(br, part, 1), &index)) {
            return false;
        }
        rec.index = index;
    }
    ok = ok && bd_kind(br, ident) == ODIN3_AST_IDENT;
    rec.name = ok ? odin3_ast_name(br->ast, ident) : 0;
    odin3_status st = bd_part_text(br, &rec, ident);
    rec.text_end = br->text.len;
    bd_part *slot = st == ODIN3_OK ? odin3_vec_push(&br->parts) : NULL;
    if (slot == NULL) {
        bd_latch(br, ODIN3_ERR_NO_MEMORY);
        return false;
    }
    *slot = rec;
    *bad = *bad || !ok;
    return true;
}

/* The components of a HIER_NAME and its path text; false after an error (reported). */
static bool bd_hier_parts(bd_run *br, odin3_ast_id node) {
    odin3_vec_clear(&br->parts);
    odin3_strbuf_clear(&br->text);
    odin3_ast_span parts = odin3_ast_children(br->ast, node);
    bool bad = false;
    for (uint32_t i = 0; i < parts.n; i++) {
        if (!bd_hier_part(br, parts.ids[i], &bad)) {
            return false;
        }
    }
    if (bad || parts.n < 2) {
        bd_error(br, node, "hierarchical reference '%s' is not supported", br->text.data);
        return false;
    }
    return true;
}

static const bd_part *bd_part_at(const bd_run *br, size_t idx) {
    return odin3_vec_cat(&br->parts, idx);
}

/* The scope reached through the generate and named blocks of every component but the last of a
 * HIER_NAME (EL-22), the first found from scope outward; 0 after its error. */
static odin3_scope_id bd_hier_scope(bd_run *br, odin3_ast_id node, odin3_scope_id scope) {
    for (size_t i = 0; i + 1 < br->parts.len; i++) {
        const bd_part *part = bd_part_at(br, i);
        odin3_sym_id sym = i == 0 ? bd_lookup(br, scope, part->name)
                                  : odin3_symtab_lookup_local(br->st, scope, part->name);
        const odin3_symbol *rec = odin3_symbol_get(br->st, sym);
        if (rec == NULL || rec->kind != ODIN3_SYM_BLOCK) {
            bd_error(br, node, "hierarchical reference '%s' is not supported", br->text.data);
            return (odin3_scope_id){0};
        }
        scope = odin3_scope_child(br->st, rec->scope, part->name, part->index);
        if (scope.v == 0) {
            bd_error(br, node, "'%s' does not exist (generate block '%.*s' was not elaborated)",
                     br->text.data, (int)part->text_end, br->text.data);
            return scope;
        }
    }
    return scope;
}

static void bd_resolve_hier(bd_run *br, const bd_ref *ref) {
    if (!bd_hier_parts(br, ref->node)) {
        return;
    }
    odin3_scope_id scope = bd_hier_scope(br, ref->node, ref->scope);
    if (scope.v == 0) {
        return;
    }
    const bd_part *last = bd_part_at(br, br->parts.len - 1);
    if (last->index != ODIN3_SCOPE_NO_INDEX) {
        bd_error(br, ref->node, "hierarchical reference '%s' is not supported", br->text.data);
        return;
    }
    bd_value_name(br, ref->node, odin3_symtab_lookup_local(br->st, scope, last->name),
                  br->text.data);
}

static void bd_resolve(bd_run *br) {
    for (size_t i = 0; i < br->refs.len && br->fatal == ODIN3_OK && !br->stopped; i++) {
        bd_ref ref = *(const bd_ref *)odin3_vec_cat(&br->refs, i);
        switch (bd_kind(br, ref.node)) {
        case ODIN3_AST_IDENT:
            bd_resolve_ident(br, &ref);
            break;
        case ODIN3_AST_HIER_NAME:
            bd_resolve_hier(br, &ref);
            break;
        case ODIN3_AST_CALL:
        case ODIN3_AST_TASK_CALL:
            bd_resolve_call(br, &ref);
            break;
        case ODIN3_AST_INSTANTIATION:
            bd_resolve_inst(br, ref.node);
            break;
        default:
            break;
        }
    }
}

/* --- assignments ---------------------------------------------------------------------------- */

static void bd_add_assign(bd_run *br, odin3_sym_id sym, const bd_assign *assign) {
    const odin3_symbol *rec = odin3_symbol_get(br->st, sym);
    if (rec == NULL) {
        return;
    }
    const odin3_sym_assign *last = odin3_assign_get(br->st, rec->last_assign);
    if (last != NULL && last->node.v == assign->node.v) {
        return; /* {a, a} = …: one record */
    }
    bd_latch(br, odin3_symbol_add_assign(br->st, sym, assign->node, assign->kind));
}

static void bd_push_work(bd_run *br, odin3_ast_id node) {
    odin3_ast_id *slot = odin3_vec_push(&br->work);
    if (slot == NULL) {
        bd_latch(br, ODIN3_ERR_NO_MEMORY);
        return;
    }
    *slot = node;
}

/* Every name an lvalue assigns: through selects and concatenations (left to right). */
static void bd_lvalue(bd_run *br, const bd_assign *assign) {
    odin3_vec_clear(&br->work);
    bd_push_work(br, assign->lhs);
    while (br->work.len > 0 && br->fatal == ODIN3_OK) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&br->work, br->work.len - 1);
        odin3_vec_pop(&br->work);
        switch (bd_kind(br, node)) {
        case ODIN3_AST_SELECT:
            bd_push_work(br, bd_child(br, node, 0));
            break;
        case ODIN3_AST_CONCAT: {
            odin3_ast_span parts = odin3_ast_children(br->ast, node);
            for (uint32_t i = parts.n; i-- > 0;) {
                bd_push_work(br, parts.ids[i]);
            }
            break;
        }
        default:
            bd_add_assign(br, bd_sym(br, node), assign);
            break;
        }
    }
}

static void bd_record(bd_run *br) {
    for (size_t i = 0; i < br->assigns.len && br->fatal == ODIN3_OK; i++) {
        bd_assign assign = *(const bd_assign *)odin3_vec_cat(&br->assigns, i);
        bd_lvalue(br, &assign);
    }
}

/* --- the entry ------------------------------------------------------------------------------ */

static odin3_status bd_open(bd_run *br, odin3_ast *elab, odin3_elab_binding *out) {
    br->ast = elab;
    br->design = (odin3_design *)odin3_ast_design(elab); /* the store borrows it writable */
    br->out = out;
    br->limits = odin3_elab_limits_get();
    odin3_vec_init(&br->frames, sizeof(bd_frame));
    odin3_vec_init(&br->refs, sizeof(bd_ref));
    odin3_vec_init(&br->assigns, sizeof(bd_assign));
    odin3_vec_init(&br->work, sizeof(odin3_ast_id));
    odin3_vec_init(&br->dims, sizeof(odin3_dim));
    odin3_vec_init(&br->parts, sizeof(bd_part));
    odin3_strbuf_init(&br->text);
    out->n = odin3_ast_node_end(elab).v;
    out->sym_of = odin3_util_calloc((size_t)out->n * sizeof *out->sym_of);
    out->scope_of = odin3_u64map_create(0);
    br->modules = odin3_u64map_create(0);
    br->scratch = odin3_arena_create(BD_SCRATCH_CHUNK);
    if (out->sym_of == NULL || out->scope_of == NULL || br->modules == NULL ||
        br->scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_ast_intern(elab, odin3_bytes_cstr(k_work), &br->work_lib);
    st = st != ODIN3_OK ? st : odin3_symtab_create(elab, &br->st);
    odin3_scope_spec design = {.kind = ODIN3_SCOPE_DESIGN};
    return st != ODIN3_OK ? st : odin3_scope_new(br->st, &design, &br->design_scope);
}

static void bd_close(bd_run *br) {
    odin3_vec_free(&br->frames);
    odin3_vec_free(&br->refs);
    odin3_vec_free(&br->assigns);
    odin3_vec_free(&br->work);
    odin3_vec_free(&br->dims);
    odin3_vec_free(&br->parts);
    odin3_strbuf_free(&br->text);
    odin3_u64map_destroy(br->modules);
    odin3_arena_destroy(br->scratch);
}

static const char *bd_misuse_why(const odin3_ast *elab) {
    if (odin3_ast_form_of(elab) != ODIN3_AST_FORM_ELABORATED) {
        return "the store is not ELABORATED";
    }
    if (odin3_ast_root_count(elab) != 1) {
        return "the elaborated form holds exactly one UNIT";
    }
    return odin3_ast_symtab(elab) != NULL ? "the store has a symbol table already" : NULL;
}

odin3_status odin3_elab_bind(odin3_ast *elab, odin3_elab_binding *out) {
    if (out != NULL) {
        *out = (odin3_elab_binding){0};
    }
    const char *why = elab == NULL || out == NULL ? "NULL argument" : bd_misuse_why(elab);
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab_bind: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    bd_run run;
    memset(&run, 0, sizeof run);
    bd_latch(&run, bd_open(&run, elab, out));
    odin3_ast_id unit = odin3_ast_root(elab, 0);
    if (run.fatal == ODIN3_OK) {
        bd_modules(&run, unit);
        bd_sweep(&run, unit);
        bd_resolve(&run);
    }
    if (run.fatal == ODIN3_OK && run.errors == 0 && !run.stopped) {
        bd_record(&run);
    }
    odin3_status st = run.fatal;
    if (st == ODIN3_OK && (run.misuse || run.errors > 0 || run.stopped)) {
        st = run.misuse ? ODIN3_ERR_INVALID_ARG : ODIN3_ERR_PARSE;
    }
    bd_close(&run);
    if (st != ODIN3_OK) {
        odin3_elab_binding_free(out);
    }
    return st;
}

/* --- the binding ---------------------------------------------------------------------------- */

odin3_scope_id odin3_elab_scope_of(const odin3_elab_binding *binding, odin3_ast_id node) {
    uint64_t scope = 0;
    if (binding == NULL || binding->scope_of == NULL ||
        !odin3_u64map_get(binding->scope_of, node.v, &scope)) {
        return (odin3_scope_id){0};
    }
    return (odin3_scope_id){(uint32_t)scope};
}

odin3_sym_id odin3_elab_sym_of(const odin3_elab_binding *binding, odin3_ast_id node) {
    if (binding == NULL || binding->sym_of == NULL || node.v >= binding->n) {
        return (odin3_sym_id){0};
    }
    return binding->sym_of[node.v];
}

void odin3_elab_binding_free(odin3_elab_binding *binding) {
    if (binding == NULL) {
        return;
    }
    odin3_util_free(binding->sym_of);
    odin3_u64map_destroy(binding->scope_of);
    *binding = (odin3_elab_binding){0};
}
