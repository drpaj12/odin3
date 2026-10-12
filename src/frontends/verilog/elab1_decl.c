/*
 * elab1_decl.c — declarations, ports and implicit nets of stage 1 (2D Task 15; EL-4, EL-23 stage 1,
 * EL-24, EL-25): the `default_nettype in effect for each module (stream order), the ports of a
 * specialization merged from the header and the body's declarations (M1, M6, M12), and the implicit
 * nets pass E finds while it walks a module's items. Every traversal uses an explicit stack.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/elab.h"
#include "odin3/odin3.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { D_SLOT_PORTS = 1, D_ITEMS = 2, D_NET_RANGE_SLOT = 1, D_TEXT = 512 };

static const uint32_t D_NONE = UINT32_MAX;

/* --- `default_nettype ------------------------------------------------------------------------ */

typedef struct d_nettype {
    const char *text;
    uint8_t sub;
} d_nettype;

static const d_nettype k_nettypes[] = {
    {"wire", ODIN3_AST_NET_DECL_WIRE},     {"tri", ODIN3_AST_NET_DECL_TRI},
    {"tri0", ODIN3_AST_NET_DECL_TRI0},     {"tri1", ODIN3_AST_NET_DECL_TRI1},
    {"wand", ODIN3_AST_NET_DECL_WAND},     {"triand", ODIN3_AST_NET_DECL_TRIAND},
    {"wor", ODIN3_AST_NET_DECL_WOR},       {"trior", ODIN3_AST_NET_DECL_TRIOR},
    {"trireg", ODIN3_AST_NET_DECL_TRIREG}, {"uwire", ODIN3_AST_NET_DECL_UWIRE},
    {"none", ODIN3_NETTYPE_NONE},
};

/* The setting a DIRECTIVE leaves, or cur when it is none of ours. */
static uint8_t directive_type(const odin3_elab1 *e1, odin3_ast_id node, uint8_t cur) {
    uint32_t sub = odin3_ast_sub(e1->parsed, node);
    if (sub == ODIN3_AST_DIRECTIVE_RESETALL) {
        return ODIN3_AST_NET_DECL_WIRE;
    }
    if (sub != ODIN3_AST_DIRECTIVE_DEFAULT_NETTYPE) {
        return cur;
    }
    const char *text = odin3_elab1_str(e1, odin3_ast_name(e1->parsed, node));
    for (size_t i = 0; text != NULL && i < sizeof k_nettypes / sizeof k_nettypes[0]; i++) {
        if (strcmp(text, k_nettypes[i].text) == 0) {
            return k_nettypes[i].sub;
        }
    }
    return cur;
}

/* The directives among the items of a MODULE, applied in order; one is a warning. */
static uint8_t module_directives(odin3_elab1 *e1, odin3_ast_id module, uint8_t cur) {
    odin3_ast_span kids = odin3_ast_children(e1->parsed, module);
    for (uint32_t i = D_ITEMS; i < kids.n; i++) {
        if (odin3_ast_kind_of(e1->parsed, kids.ids[i]) != ODIN3_AST_DIRECTIVE) {
            continue;
        }
        uint8_t next = directive_type(e1, kids.ids[i], cur);
        if (odin3_ast_sub(e1->parsed, kids.ids[i]) == ODIN3_AST_DIRECTIVE_DEFAULT_NETTYPE) {
            odin3_elab1_warn_once(
                e1, kids.ids[i],
                "`default_nettype inside a module takes effect at the next module");
        }
        cur = next;
    }
    return cur;
}

static odin3_status build_nettypes(odin3_elab1 *e1) {
    e1->nettypes = odin3_u64map_create(0);
    if (e1->nettypes == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint8_t cur = ODIN3_AST_NET_DECL_WIRE;
    odin3_status st = ODIN3_OK;
    for (uint32_t unit = 0; st == ODIN3_OK && unit < odin3_ast_root_count(e1->parsed); unit++) {
        odin3_ast_span kids = odin3_ast_children(e1->parsed, odin3_ast_root(e1->parsed, unit));
        for (uint32_t i = 0; st == ODIN3_OK && i < kids.n; i++) {
            odin3_ast_kind kind = odin3_ast_kind_of(e1->parsed, kids.ids[i]);
            if (kind == ODIN3_AST_DIRECTIVE) {
                cur = directive_type(e1, kids.ids[i], cur);
            } else if (kind == ODIN3_AST_MODULE) {
                st = odin3_u64map_put(e1->nettypes, (odin3_kv){kids.ids[i].v, cur});
                cur = module_directives(e1, kids.ids[i], cur);
            }
        }
    }
    return st;
}

uint8_t odin3_elab1_nettype_at(odin3_elab1 *e1, odin3_ast_id module_node) {
    if (e1->nettypes == NULL) {
        odin3_status st = build_nettypes(e1);
        if (st != ODIN3_OK) {
            e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
            return ODIN3_AST_NET_DECL_WIRE;
        }
    }
    uint64_t type = ODIN3_AST_NET_DECL_WIRE;
    return odin3_u64map_get(e1->nettypes, module_node.v, &type) ? (uint8_t)type
                                                                : (uint8_t)ODIN3_AST_NET_DECL_WIRE;
}

uint8_t odin3_elab1_implicit_sub(uint8_t nettype) {
    switch (nettype) {
    case ODIN3_AST_NET_DECL_WAND:
    case ODIN3_AST_NET_DECL_TRIAND:
        return ODIN3_AST_NET_DECL_WAND;
    case ODIN3_AST_NET_DECL_WOR:
    case ODIN3_AST_NET_DECL_TRIOR:
        return ODIN3_AST_NET_DECL_WOR;
    case ODIN3_AST_NET_DECL_UWIRE:
        return ODIN3_AST_NET_DECL_UWIRE;
    default:
        return ODIN3_AST_NET_DECL_WIRE;
    }
}

bool odin3_elab1_directive_handled(const odin3_elab1 *e1, odin3_ast_id directive) {
    uint32_t sub = odin3_ast_sub(e1->parsed, directive);
    return sub == ODIN3_AST_DIRECTIVE_DEFAULT_NETTYPE || sub == ODIN3_AST_DIRECTIVE_RESETALL;
}

void odin3_elab1_decl_free(odin3_elab1_env *env) {
    odin3_vec_free(&env->ports);
    odin3_vec_free(&env->implicit);
    odin3_vec_free(&env->scan);
    odin3_u64map_destroy(env->imp_names);
    odin3_u64map_destroy(env->locals);
    env->imp_names = NULL;
    env->locals = NULL;
}

/* --- ports (EL-24) --------------------------------------------------------------------------- */

static void d_error(const odin3_elab1_env *env, odin3_ast_id at, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

static void d_error(const odin3_elab1_env *env, odin3_ast_id at, const char *fmt, ...) {
    char text[D_TEXT];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    odin3_elab1_report(env->e1, ODIN3_LOG_ERROR, odin3_ast_loc(env->e1->parsed, at),
                       len >= 0 && (size_t)len < sizeof text ? text : fmt);
}

static uint32_t flag_field(uint32_t flags, uint32_t mask, uint32_t shift) {
    return (flags & mask) >> shift;
}

/* A PORT_DECL's own type fields as an mport (the ANSI form; a non-ANSI port adds its net/var). */
static odin3_elab1_mport port_of(const odin3_ast *ast, odin3_ast_id group, odin3_ast_id decl) {
    uint32_t flags = odin3_ast_flags(ast, group);
    return (odin3_elab1_mport){
        .name = odin3_ast_name(ast, decl),
        .dir = (uint8_t)odin3_ast_sub(ast, group),
        .dt = (uint8_t)flag_field(flags, ODIN3_AST_F_PORT_DECL_DT_MASK,
                                  ODIN3_AST_F_PORT_DECL_DT_SHIFT),
        .net = (uint8_t)flag_field(flags, ODIN3_AST_F_PORT_DECL_NET_MASK,
                                   ODIN3_AST_F_PORT_DECL_NET_SHIFT),
        .is_signed = (flags & ODIN3_AST_F_PORT_DECL_SIGNED) != 0,
        .range = odin3_ast_child(ast, group, 0),
        .group = group,
        .decl = decl,
    };
}

/* M12 and EL-24's untyped ports: an `input reg`/`input integer` is a net (warned), a port without
 * a type is a `wire`. */
static void port_final(odin3_elab1_env *env, odin3_elab1_mport *port) {
    if (port->dir == ODIN3_AST_PORT_DECL_INPUT && port->dt != ODIN3_AST_DT_NONE) {
        odin3_elab1_warn_once(env->e1, port->decl,
                              "input '%s' is declared as a variable ('%s'); it is treated as a net",
                              odin3_elab1_str(env->e1, port->name),
                              port->dt == ODIN3_AST_DT_INTEGER ? "integer" : "reg");
        port->as_integer = port->dt == ODIN3_AST_DT_INTEGER;
        port->is_signed = port->is_signed || port->as_integer;
        port->dt = ODIN3_AST_DT_NONE;
    }
    if (port->dt == ODIN3_AST_DT_NONE && port->net == 0) {
        port->net = 1U + (uint8_t)ODIN3_AST_NET_DECL_WIRE;
    }
}

static odin3_status push_port(odin3_elab1_env *env, const odin3_elab1_mport *port) {
    odin3_elab1_mport *slot = odin3_vec_push(&env->ports);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *port;
    return ODIN3_OK;
}

static bool is_object_group(const odin3_ast *ast, odin3_ast_id group) {
    odin3_ast_kind kind = group.v != 0 ? odin3_ast_kind_of(ast, group) : ODIN3_AST_LIST;
    return kind == ODIN3_AST_NET_DECL || kind == ODIN3_AST_VAR_DECL;
}

/* The RANGE of a NET_DECL or VAR_DECL. */
static odin3_ast_id object_range(const odin3_ast *ast, odin3_ast_id group) {
    return odin3_ast_child(
        ast, group, odin3_ast_kind_of(ast, group) == ODIN3_AST_NET_DECL ? D_NET_RANGE_SLOT : 0);
}

/* M1: the written ranges of a port and its net or variable declaration must agree. */
static odin3_status ranges_agree(odin3_elab1_env *env, const odin3_elab1_mport *port,
                                 const odin3_ast_id ranges[2], odin3_ast_id second_decl) {
    int64_t one[2] = {0, 0};
    int64_t two[2] = {0, 0};
    odin3_status st = odin3_elab1_env_range(env, ranges[0], &one[0], &one[1]);
    st = st != ODIN3_OK ? st : odin3_elab1_env_range(env, ranges[1], &two[0], &two[1]);
    if (st == ODIN3_ERR_PARSE) {
        return ODIN3_OK; /* reported at the bound */
    }
    if (st == ODIN3_OK && (one[0] != two[0] || one[1] != two[1])) {
        d_error(env, second_decl,
                "port '%s' is declared with ranges [%" PRId64 ":%" PRId64 "] and [%" PRId64
                ":%" PRId64 "]",
                odin3_elab1_str(env->e1, port->name), one[0], one[1], two[0], two[1]);
    }
    return st;
}

/* The type fields a merged net or variable declaration gives the port (N15 and N17 are rejected
 * at the declaration). */
static void merge_type(odin3_elab1_env *env, odin3_ast_id obj, odin3_elab1_mport *port) {
    const odin3_ast *ast = env->e1->parsed;
    uint32_t sub = odin3_ast_sub(ast, obj);
    if (odin3_ast_kind_of(ast, obj) == ODIN3_AST_NET_DECL) {
        port->net = port->net != 0 ? port->net : (uint8_t)(1U + sub);
        if (sub >= ODIN3_AST_NET_DECL_TRI0 && sub <= ODIN3_AST_NET_DECL_TRIREG) {
            (void)odin3_elab1_reject(env->e1, obj); /* N15 */
        }
    } else if (sub == ODIN3_AST_VAR_DECL_REG || sub == ODIN3_AST_VAR_DECL_INTEGER) {
        uint8_t dt = sub == ODIN3_AST_VAR_DECL_REG ? ODIN3_AST_DT_REG : ODIN3_AST_DT_INTEGER;
        port->dt = port->dt != ODIN3_AST_DT_NONE ? port->dt : dt;
    } else {
        (void)odin3_elab1_reject(env->e1, obj); /* N17: a time or real variable */
    }
    port->is_signed =
        port->is_signed || (odin3_ast_flags(ast, obj) & ODIN3_AST_F_NET_DECL_SIGNED) != 0;
}

/* A non-ANSI port merged with the net or variable declared beside it (the slot's two groups). */
static odin3_status merge_object(odin3_elab1_env *env, const odin3_elab1_slot *slot,
                                 odin3_elab1_mport *port) {
    const odin3_ast *ast = env->e1->parsed;
    bool port_first = odin3_ast_kind_of(ast, slot->group) == ODIN3_AST_PORT_DECL;
    odin3_ast_id obj = port_first ? slot->group2 : slot->group;
    if (obj.v == 0 || !is_object_group(ast, obj)) {
        return ODIN3_OK;
    }
    merge_type(env, obj, port);
    odin3_ast_id other = object_range(ast, obj);
    odin3_status st = ODIN3_OK;
    if (port->range.v != 0 && other.v != 0) {
        const odin3_ast_id ranges[2] = {port_first ? port->range : other,
                                        port_first ? other : port->range};
        st = ranges_agree(env, port, ranges, slot->decl2);
    }
    port->range = port->range.v != 0 ? port->range : other;
    return st;
}

/* An ANSI port redeclared as a net or variable in the body (`module m(output q); reg q;`). */
static odin3_status merge_header(odin3_elab1_env *env, odin3_elab1_mport *port) {
    uint32_t idx = odin3_elab1_env_find(env, 0, port->name);
    const odin3_elab1_slot *slot = idx != D_NONE ? odin3_elab1_env_slot(env, idx) : NULL;
    if (slot == NULL || slot->entry.kind != ODIN3_ENTRY_SIGNAL || slot->group2.v == 0 ||
        slot->entry.decl.v != port->decl.v) {
        return ODIN3_OK;
    }
    return merge_object(env, slot, port);
}

/* A non-ANSI header name: its body PORT_DECL merged with its net or variable declaration. */
static odin3_status header_name(odin3_elab1_env *env, odin3_ast_id ref) {
    const odin3_ast *ast = env->e1->parsed;
    uint32_t name = odin3_ast_name(ast, ref);
    uint32_t idx = odin3_elab1_env_find(env, 0, name);
    const odin3_elab1_slot *slot = idx != D_NONE ? odin3_elab1_env_slot(env, idx) : NULL;
    odin3_ast_id group = {0};
    odin3_ast_id decl = {0};
    if (slot != NULL && slot->entry.kind == ODIN3_ENTRY_SIGNAL) {
        bool first = odin3_ast_kind_of(ast, slot->group) == ODIN3_AST_PORT_DECL;
        group = first ? slot->group : slot->group2;
        decl = first ? slot->entry.decl : slot->decl2;
    }
    if (group.v == 0 || odin3_ast_kind_of(ast, group) != ODIN3_AST_PORT_DECL) {
        d_error(env, ref, "port '%s' is not declared", odin3_elab1_str(env->e1, name));
        return ODIN3_OK;
    }
    odin3_elab1_mport port = port_of(ast, group, decl);
    odin3_status st = merge_object(env, slot, &port);
    port_final(env, &port);
    return st != ODIN3_OK ? st : push_port(env, &port);
}

/* A PORT_REF of the header: an empty position, a plain name or M6's port expression. */
static odin3_status header_ref(odin3_elab1_env *env, odin3_ast_id ref, bool *expr_seen) {
    const odin3_ast *ast = env->e1->parsed;
    bool expr = odin3_ast_child(ast, ref, 0).v != 0 ||
                (odin3_ast_flags(ast, ref) & ODIN3_AST_F_PORT_REF_DOTTED) != 0;
    if (expr) {
        d_error(env, ref, "port expression in a module port list is not supported");
        *expr_seen = true;
        return ODIN3_OK;
    }
    if (odin3_ast_name(ast, ref) == 0) {
        odin3_elab1_mport empty = {.decl = ref};
        return push_port(env, &empty);
    }
    return header_name(env, ref);
}

/* The header's names that body PORT_DECLs may declare (name → 1). */
static odin3_status header_names(const odin3_elab1_env *env, odin3_u64map *names) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span entries =
        odin3_ast_children(ast, odin3_ast_child(ast, env->module, D_SLOT_PORTS));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < entries.n; i++) {
        if (odin3_ast_kind_of(ast, entries.ids[i]) == ODIN3_AST_PORT_REF) {
            st = odin3_u64map_put(names, (odin3_kv){odin3_ast_name(ast, entries.ids[i]), 1});
        }
    }
    return st;
}

/* A body PORT_DECL whose names must be in the header. */
static void unlisted(odin3_elab1_env *env, odin3_ast_id group, const odin3_u64map *names) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span decls = odin3_ast_children(ast, group);
    for (uint32_t j = odin3_ast_slot_count(ODIN3_AST_PORT_DECL); j < decls.n; j++) {
        if (!odin3_u64map_get(names, odin3_ast_name(ast, decls.ids[j]), NULL)) {
            d_error(env, decls.ids[j], "'%s' is declared as a port but is not in the port list",
                    odin3_elab1_str(env->e1, odin3_ast_name(ast, decls.ids[j])));
        }
    }
}

/* An ANSI PORT_DECL of the header: one merged port per declarator. */
static odin3_status ansi_ports(odin3_elab1_env *env, odin3_ast_id group) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span decls = odin3_ast_children(ast, group);
    odin3_status st = ODIN3_OK;
    for (uint32_t j = odin3_ast_slot_count(ODIN3_AST_PORT_DECL); st == ODIN3_OK && j < decls.n;
         j++) {
        odin3_elab1_mport port = port_of(ast, group, decls.ids[j]);
        port.ansi = true;
        st = merge_header(env, &port);
        port_final(env, &port);
        st = st != ODIN3_OK ? st : push_port(env, &port);
    }
    return st;
}

odin3_status odin3_elab1_ports(odin3_elab1 *e1, uint32_t spec) {
    (void)spec;
    odin3_elab1_env *env = e1->env;
    const odin3_ast *ast = e1->parsed;
    odin3_ast_span entries =
        odin3_ast_children(ast, odin3_ast_child(ast, env->module, D_SLOT_PORTS));
    odin3_u64map *names = odin3_u64map_create(0);
    bool expr_seen = false; /* a port expression may use any name: M6 is the one error */
    odin3_status st = names != NULL ? header_names(env, names) : ODIN3_ERR_NO_MEMORY;
    for (uint32_t i = 0; st == ODIN3_OK && i < entries.n; i++) {
        st = odin3_ast_kind_of(ast, entries.ids[i]) == ODIN3_AST_PORT_REF
                 ? header_ref(env, entries.ids[i], &expr_seen)
                 : ansi_ports(env, entries.ids[i]);
    }
    odin3_ast_span kids = odin3_ast_children(ast, env->module);
    for (uint32_t i = D_ITEMS; st == ODIN3_OK && i < kids.n; i++) {
        if (!expr_seen && odin3_ast_kind_of(ast, kids.ids[i]) == ODIN3_AST_PORT_DECL) {
            unlisted(env, kids.ids[i], names);
        }
    }
    odin3_u64map_destroy(names);
    return st;
}

/* --- implicit nets (EL-25) ------------------------------------------------------------------- */

/* The use count of a name local to a block or subroutine being scanned (enter: +1, else -1). */
static odin3_status local_add(odin3_u64map *map, uint32_t name, bool enter) {
    uint64_t count = 0;
    (void)odin3_u64map_get(map, name, &count);
    if (enter) {
        count++;
    } else if (count > 0) {
        count--;
    }
    return odin3_u64map_put(map, (odin3_kv){name, count});
}

/* The declarators of the groups of the LIST in slot idx of node. */
static odin3_status list_names(odin3_u64map *map, const odin3_ast *ast, odin3_ast_id node,
                               uint32_t idx, bool enter) {
    odin3_ast_id list = odin3_ast_child(ast, node, idx);
    odin3_ast_span groups = odin3_ast_children(ast, list);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; list.v != 0 && st == ODIN3_OK && i < groups.n; i++) {
        odin3_ast_span decls = odin3_ast_children(ast, groups.ids[i]);
        for (uint32_t j = odin3_ast_slot_count(odin3_ast_kind_of(ast, groups.ids[i]));
             st == ODIN3_OK && j < decls.n; j++) {
            st = local_add(map, odin3_ast_name(ast, decls.ids[j]), enter);
        }
    }
    return st;
}

/* The names a block or subroutine declares for its body (its own name too, for a subroutine). */
odin3_status odin3_elab1_scope_names(odin3_u64map *map, const odin3_ast *ast, odin3_ast_id node,
                                     bool enter) {
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_SEQ_BLOCK:
        return list_names(map, ast, node, 0, enter);
    case ODIN3_AST_FUNCTION_DECL: {
        odin3_status st = local_add(map, odin3_ast_name(ast, node), enter);
        st = st != ODIN3_OK ? st : list_names(map, ast, node, 1, enter);
        return st != ODIN3_OK ? st : list_names(map, ast, node, 2, enter);
    }
    default: { /* TASK_DECL */
        odin3_status st = local_add(map, odin3_ast_name(ast, node), enter);
        st = st != ODIN3_OK ? st : list_names(map, ast, node, 0, enter);
        return st != ODIN3_OK ? st : list_names(map, ast, node, 1, enter);
    }
    }
}

/* Whether kind keeps its first child out of the scan (a constant, a range, a parameter list). */
static bool first_skipped(odin3_ast_kind kind) {
    return kind == ODIN3_AST_REPLICATE || kind == ODIN3_AST_REPEAT ||
           kind == ODIN3_AST_INSTANTIATION || kind == ODIN3_AST_INSTANCE ||
           kind == ODIN3_AST_FUNCTION_DECL;
}

/* Whether kind keeps child idx out of the scan: a constant position (EL-25's exceptions), a
 * declaration's range, a delay or strength slot. */
static bool slot_skipped(const odin3_ast *ast, odin3_ast_id node, odin3_ast_kind kind,
                         uint32_t idx) {
    if (first_skipped(kind) && idx == 0) {
        return true;
    }
    switch (kind) {
    case ODIN3_AST_SELECT: {
        uint32_t sub = odin3_ast_sub(ast, node);
        return (sub == ODIN3_AST_SELECT_PART && idx >= 1) ||
               (sub != ODIN3_AST_SELECT_PART && sub != ODIN3_AST_SELECT_BIT && idx == 2);
    }
    case ODIN3_AST_DECLARATOR:
        return idx >= 1;
    case ODIN3_AST_GATE_DECL:
    case ODIN3_AST_CONT_ASSIGN:
        return idx < 2;
    case ODIN3_AST_NET_DECL:
    case ODIN3_AST_VAR_DECL:
    case ODIN3_AST_PORT_DECL:
        return idx < odin3_ast_slot_count(kind);
    default:
        return false;
    }
}

/* Whether the scan leaves child idx of node (a kid of kind) alone. */
static bool scan_skips(const odin3_ast *ast, odin3_ast_id node, uint32_t idx, odin3_ast_id kid) {
    switch (odin3_ast_kind_of(ast, kid)) {
    case ODIN3_AST_RANGE:
    case ODIN3_AST_DELAY:
    case ODIN3_AST_STRENGTH:
    case ODIN3_AST_MINTYPMAX:
    case ODIN3_AST_HIER_NAME:
        return true;
    default:
        return slot_skipped(ast, node, odin3_ast_kind_of(ast, node), idx);
    }
}

static odin3_status push_frame(odin3_elab1_env *env, odin3_ast_id node, bool leave) {
    odin3_elab1_scan_frame *frame = odin3_vec_push(&env->scan);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (odin3_elab1_scan_frame){node, leave};
    return ODIN3_OK;
}

/* The scanned children of node pushed so the first is popped first (a CASE_ITEM's labels before
 * its body, as written). */
static odin3_status push_kids(odin3_elab1_env *env, odin3_ast_id node) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_kind kind = odin3_ast_kind_of(ast, node);
    odin3_ast_span kids = odin3_ast_children(ast, node);
    if (kind == ODIN3_AST_CALL && (odin3_ast_flags(ast, node) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        const char *name = odin3_elab1_str(env->e1, odin3_ast_name(ast, node));
        if (name != NULL && strcmp(name, "$clog2") == 0) {
            return ODIN3_OK; /* its argument is a constant position */
        }
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = kids.n; st == ODIN3_OK && k > 0; k--) {
        uint32_t idx = k - 1;
        if (kind == ODIN3_AST_CASE_ITEM) {
            idx = k == kids.n ? 0 : k; /* the order is 1 .. n - 1, then 0 */
        }
        odin3_ast_id kid = kids.ids[idx];
        if (kid.v != 0 && !scan_skips(ast, node, idx, kid)) {
            st = push_frame(env, kid, false);
        }
    }
    return st;
}

/* The text of a net type (`default_nettype argument). */
static const char *nettype_name(uint8_t sub) {
    for (size_t i = 0; i < sizeof k_nettypes / sizeof k_nettypes[0]; i++) {
        if (k_nettypes[i].sub == sub) {
            return k_nettypes[i].text;
        }
    }
    return "";
}

/* The implicit net of an undeclared name, or the error its `default_nettype gives. */
static odin3_status implicit_net(odin3_elab1_env *env, odin3_ast_id use, uint32_t name) {
    odin3_elab1 *e1 = env->e1;
    const char *text = odin3_elab1_str(e1, name);
    odin3_status st = odin3_u64map_put(env->imp_names, (odin3_kv){name, 1});
    if (st != ODIN3_OK) {
        return st;
    }
    uint8_t type = env->nettype;
    if (type == ODIN3_NETTYPE_NONE) {
        d_error(env, use, "'%s' is not declared (`default_nettype is none)", text);
        return ODIN3_OK;
    }
    if (type == ODIN3_AST_NET_DECL_TRI0 || type == ODIN3_AST_NET_DECL_TRI1 ||
        type == ODIN3_AST_NET_DECL_TRIREG) {
        d_error(env, use, "net type '%s' is not supported", nettype_name(type));
        return ODIN3_OK;
    }
    odin3_elab1_implicit_net *rec = odin3_vec_push(&env->implicit);
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rec = (odin3_elab1_implicit_net){name, use};
    odin3_elab1_warn_once(e1, use, "'%s' is implicitly declared", text);
    return ODIN3_OK;
}

/* An IDENT: declared in the scope chain, a local, or an earlier implicit net; else implicit. */
static odin3_status scan_ident(odin3_elab1_env *env, odin3_ast_id ident) {
    uint32_t name = odin3_ast_name(env->e1->parsed, ident);
    if (name == 0 || odin3_elab1_env_lookup(env, name) != D_NONE ||
        odin3_u64map_get(env->imp_names, name, NULL)) {
        return ODIN3_OK;
    }
    uint64_t count = 0;
    if (odin3_u64map_get(env->locals, name, &count) && count > 0) {
        return ODIN3_OK;
    }
    return implicit_net(env, ident, name);
}

static bool scanned_item(odin3_ast_kind kind) {
    switch (kind) {
    case ODIN3_AST_NET_DECL:
    case ODIN3_AST_VAR_DECL:
    case ODIN3_AST_CONT_ASSIGN:
    case ODIN3_AST_ALWAYS:
    case ODIN3_AST_INITIAL:
    case ODIN3_AST_INSTANTIATION:
    case ODIN3_AST_GATE_DECL:
    case ODIN3_AST_FUNCTION_DECL:
    case ODIN3_AST_TASK_DECL:
        return true;
    default:
        return false;
    }
}

/* One frame of the scan: the end of a scope, a name, a scope opened, or the kids of a node. */
static odin3_status scan_step(odin3_elab1_env *env, const odin3_elab1_scan_frame *frame) {
    odin3_ast_kind kind = odin3_ast_kind_of(env->e1->parsed, frame->node);
    if (frame->leave) {
        return odin3_elab1_scope_names(env->locals, env->e1->parsed, frame->node, false);
    }
    if (kind == ODIN3_AST_IDENT) {
        return scan_ident(env, frame->node);
    }
    if (kind != ODIN3_AST_SEQ_BLOCK && kind != ODIN3_AST_FUNCTION_DECL &&
        kind != ODIN3_AST_TASK_DECL) {
        return push_kids(env, frame->node);
    }
    odin3_status st = odin3_elab1_scope_names(env->locals, env->e1->parsed, frame->node, true);
    st = st != ODIN3_OK ? st : push_frame(env, frame->node, true);
    return st != ODIN3_OK ? st : push_kids(env, frame->node);
}

odin3_status odin3_elab1_implicit(odin3_elab1_env *env, odin3_ast_id item) {
    if (!scanned_item(odin3_ast_kind_of(env->e1->parsed, item))) {
        return ODIN3_OK;
    }
    if (env->imp_names == NULL) {
        env->imp_names = odin3_u64map_create(0);
        env->locals = odin3_u64map_create(0);
        if (env->imp_names == NULL || env->locals == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    odin3_status st = push_frame(env, item, false);
    while (st == ODIN3_OK && env->scan.len > 0) {
        odin3_elab1_scan_frame frame =
            *(const odin3_elab1_scan_frame *)odin3_vec_cat(&env->scan, env->scan.len - 1);
        odin3_vec_pop(&env->scan);
        st = scan_step(env, &frame);
    }
    odin3_vec_clear(&env->scan);
    return st;
}
