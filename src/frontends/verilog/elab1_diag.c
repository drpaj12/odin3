/*
 * elab1_diag.c — the REJECT and IGNORE rows of stage 1 (2D spec §9.1, §9.2; EL-42, EL-43). One
 * scan per definition walks the MODULE's whole parsed body in source order (an explicit frame
 * stack, no recursion) and reports each row at the node §9 names: the REJECT rows as located
 * errors counted under the error cap (§9.3), the IGNORE rows as warnings once per parsed node.
 * The rows that depend on the enclosing construct (T12, K12 and F2) are checked with the context
 * the walk keeps (the function being walked, the TIMING_STMT an ALWAYS owns). A definition whose
 * scan reported an error is not expanded or written (EL-44: what depends on a failed item is
 * skipped silently). The elaborated form drops what is ignored; pass W does that, silently.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    D_DELAY_SLOT_NET = 2, /* NET_DECL(STRENGTH?, RANGE?, DELAY?) */
    D_STRENGTH_SLOT = 0,
    D_DELAY_SLOT_CONT = 1, /* CONT_ASSIGN and GATE_DECL: (STRENGTH?, DELAY?) */
    D_CONTROL_SLOT = 1     /* BLOCKING_ASSIGN(lhs, TIMING?, rhs) */
};

/* The context the walk keeps. */
typedef struct diag_scan {
    odin3_elab1 *e1;
    const odin3_ast *ast;
    const char *fn_name; /* the FUNCTION_DECL being walked, NULL outside one */
    odin3_ast_id top;    /* the TIMING_STMT the ALWAYS being walked owns */
    bool initial;        /* inside an INITIAL */
    odin3_vec frames;    /* diag_frame */
} diag_scan;

typedef struct diag_frame {
    uint32_t node;
    uint32_t next;
} diag_frame;

static odin3_ast_id kid(const odin3_ast *ast, odin3_ast_id node, uint32_t idx) {
    return idx < odin3_ast_nchild(ast, node) ? odin3_ast_child(ast, node, idx) : (odin3_ast_id){0};
}

static bool is_kind(const odin3_ast *ast, odin3_ast_id node, odin3_ast_kind kind) {
    return node.v != 0 && odin3_ast_kind_of(ast, node) == kind;
}

/* --- the REJECT rows -------------------------------------------------------------------------- */

static bool rej(odin3_elab1 *e1, odin3_ast_id node, const char *fmt, ...) ODIN3_PRINTF(3, 4);

static bool rej(odin3_elab1 *e1, odin3_ast_id node, const char *fmt, ...) {
    char text[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    if (vsnprintf(text, sizeof text, fmt, args) < 0) {
        text[0] = '\0';
    }
    va_end(args);
    odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, node), "%s", text);
    return true;
}

/* The net kind (an odin3_ast_net_kind) a PORT_DECL names, or UINT32_MAX when it names none. */
static uint32_t port_net_kind(const odin3_ast *ast, odin3_ast_id port) {
    uint32_t field =
        ((uint32_t)odin3_ast_flags(ast, port) & (uint32_t)ODIN3_AST_F_PORT_DECL_NET_MASK) >>
        (uint32_t)ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    return field == 0 ? UINT32_MAX : field - 1U;
}

static const char *const k_pull_names[] = {"tri0", "tri1", "trireg"};

static bool pull_net(uint32_t kind) {
    return kind >= ODIN3_AST_NET_DECL_TRI0 && kind <= ODIN3_AST_NET_DECL_TRIREG;
}

/* L9, N15, N17, N18, Q6: expressions and declarations. */
static bool reject_decl(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_REAL:
        return rej(e1, node, "real number in an expression is not supported");
    case ODIN3_AST_VAR_DECL:
        return odin3_ast_sub(ast, node) >= ODIN3_AST_VAR_DECL_TIME &&
               rej(e1, node, "variable type '%s' is not supported", odin3_ast_sub_name(ast, node));
    case ODIN3_AST_NET_DECL:
        return pull_net(odin3_ast_sub(ast, node)) &&
               rej(e1, node, "net type '%s' is not supported", odin3_ast_sub_name(ast, node));
    case ODIN3_AST_PORT_DECL:
        return pull_net(port_net_kind(ast, node)) &&
               rej(e1, node, "net type '%s' is not supported",
                   k_pull_names[port_net_kind(ast, node) - ODIN3_AST_NET_DECL_TRI0]);
    case ODIN3_AST_EVENT_DECL: {
        bool bad = false;
        odin3_ast_span decls = odin3_ast_children(ast, node);
        for (uint32_t i = 0; i < decls.n; i++) {
            bad = rej(e1, node, "named event '%s' is not supported",
                      odin3_ast_name_str(ast, decls.ids[i])) ||
                  bad;
        }
        return bad;
    }
    case ODIN3_AST_GATE_DECL: {
        uint32_t sub = odin3_ast_sub(ast, node);
        const char *name = odin3_ast_sub_name(ast, node);
        if (sub < ODIN3_AST_GATE_DECL_NMOS) {
            return false;
        }
        return sub >= ODIN3_AST_GATE_DECL_PULLUP
                   ? rej(e1, node, "'%s' is not supported", name)
                   : rej(e1, node, "switch-level primitive '%s' is not supported", name);
    }
    default:
        return false;
    }
}

/* T13, T14, T15, T16, T19, F4: statements that are never supported. */
static bool reject_stmt(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_WAIT:
        return rej(e1, node, "'wait' is not supported");
    case ODIN3_AST_EVENT_TRIGGER:
        return rej(e1, node, "event trigger '->' is not supported");
    case ODIN3_AST_PAR_BLOCK:
        return rej(e1, node, "'fork ... join' is not supported");
    case ODIN3_AST_PROC_CONT_ASSIGN:
        return rej(e1, node, "procedural '%s' is not supported", odin3_ast_sub_name(ast, node));
    case ODIN3_AST_DISABLE:
        return rej(e1, node, "'disable' is not supported");
    case ODIN3_AST_FOREVER:
        return rej(e1, node, "'forever' is not supported");
    default:
        return false;
    }
}

/* T6: an always block must start with an event control (`always_comb` and the like need none). */
static bool reject_always(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    odin3_ast_id first = kid(ast, node, 0);
    bool ok = odin3_ast_sub(ast, node) != ODIN3_AST_ALWAYS_ALWAYS ||
              (is_kind(ast, first, ODIN3_AST_TIMING_STMT) &&
               is_kind(ast, kid(ast, first, 0), ODIN3_AST_EVENT_CONTROL));
    return !ok && rej(e1, node, "always block without an event control is not supported");
}

/* An event list holds edge events or level events, not both. */
static bool reject_events(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    bool edge = false;
    bool level = false;
    odin3_ast_span events = odin3_ast_children(ast, node);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_EVENT_CONTROL); i < events.n; i++) {
        bool is_level = odin3_ast_sub(ast, events.ids[i]) == ODIN3_AST_EVENT_EXPR_NONE;
        edge = edge || !is_level;
        level = level || is_level;
    }
    return edge && level &&
           rej(e1, node, "edge and level events in one event list are not supported");
}

bool odin3_elab1_system_kept(odin3_ast_kind kind, const char *name) {
    if (name == NULL) {
        return false;
    }
    if (kind == ODIN3_AST_CALL) {
        return strcmp(name, "$signed") == 0 || strcmp(name, "$unsigned") == 0 ||
               strcmp(name, "$clog2") == 0;
    }
    return kind == ODIN3_AST_TASK_CALL &&
           (strcmp(name, "$readmemh") == 0 || strcmp(name, "$readmemb") == 0);
}

static bool read_task(const char *name) {
    return odin3_elab1_system_kept(ODIN3_AST_TASK_CALL, name);
}

/* E20 and E21: a system function that is not kept, a hierarchical call. */
static bool reject_call(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    uint32_t flags = odin3_ast_flags(ast, node);
    const char *name = odin3_ast_name_str(ast, node);
    if (odin3_ast_kind_of(ast, node) == ODIN3_AST_CALL) {
        if ((flags & ODIN3_AST_F_CALL_HIER) != 0) {
            return rej(e1, node, "hierarchical reference '%s' is not supported", name);
        }
        return (flags & ODIN3_AST_F_CALL_SYSTEM) != 0 &&
               !odin3_elab1_system_kept(ODIN3_AST_CALL, name) &&
               rej(e1, node, "system function '%s' is not supported", name);
    }
    return (flags & ODIN3_AST_F_TASK_CALL_HIER) != 0 &&
           rej(e1, node, "hierarchical reference '%s' is not supported", name);
}

/* The REJECT rows that need only the node. */
static bool reject_free(odin3_elab1 *e1, odin3_ast_id node) {
    switch (odin3_ast_kind_of(e1->parsed, node)) {
    case ODIN3_AST_ALWAYS:
        return reject_always(e1, node);
    case ODIN3_AST_EVENT_CONTROL:
        return reject_events(e1, node);
    case ODIN3_AST_CALL:
    case ODIN3_AST_TASK_CALL:
        return reject_call(e1, node);
    default:
        return reject_decl(e1, node) || reject_stmt(e1, node);
    }
}

odin3_status odin3_elab1_reject(odin3_elab1 *e1, odin3_ast_id node) {
    return reject_free(e1, node) ? ODIN3_ERR_PARSE : ODIN3_OK;
}

/* K12 and T12 for a TIMING_STMT that is not its always's own. */
static bool reject_timing(const diag_scan *sc, odin3_ast_id node) {
    const odin3_ast *ast = sc->ast;
    if (node.v == sc->top.v) {
        return false;
    }
    if (sc->fn_name != NULL) {
        return rej(sc->e1, node, "timing control in function '%s' is not supported", sc->fn_name);
    }
    return is_kind(ast, kid(ast, node, 0), ODIN3_AST_EVENT_CONTROL) &&
           rej(sc->e1, node, "event control inside an always body is not supported");
}

/* K12 and T12 for an assignment (a nonblocking one in a function, an intra-assignment control). */
static bool reject_assign(const diag_scan *sc, odin3_ast_id node) {
    const odin3_ast *ast = sc->ast;
    if (sc->fn_name != NULL && odin3_ast_kind_of(ast, node) == ODIN3_AST_NONBLOCKING_ASSIGN) {
        return rej(sc->e1, node, "nonblocking assignment in function '%s' is not supported",
                   sc->fn_name);
    }
    odin3_ast_id ctl = kid(ast, node, D_CONTROL_SLOT);
    if (ctl.v == 0) {
        return false;
    }
    if (sc->fn_name != NULL) {
        return rej(sc->e1, node, "timing control in function '%s' is not supported", sc->fn_name);
    }
    return is_kind(ast, ctl, ODIN3_AST_EVENT_CONTROL) &&
           rej(sc->e1, ctl, "event control inside an always body is not supported");
}

/* A $readmemh or $readmemb outside an initial block. */
static bool reject_readmem(const diag_scan *sc, odin3_ast_id node) {
    const char *name = odin3_ast_name_str(sc->ast, node);
    return !sc->initial && read_task(name) &&
           (odin3_ast_flags(sc->ast, node) & ODIN3_AST_F_TASK_CALL_SYSTEM) != 0 &&
           rej(sc->e1, node, "%s is supported only in an initial block", name);
}

/* T12, K12 and F2: the rows that depend on the construct around the node. */
static bool reject_ctx(const diag_scan *sc, odin3_ast_id node) {
    switch (odin3_ast_kind_of(sc->ast, node)) {
    case ODIN3_AST_TIMING_STMT:
        return reject_timing(sc, node);
    case ODIN3_AST_NONBLOCKING_ASSIGN:
    case ODIN3_AST_BLOCKING_ASSIGN:
        return reject_assign(sc, node);
    case ODIN3_AST_WHILE:
        return sc->fn_name == NULL &&
               rej(sc->e1, node, "'while' is supported only in constant functions");
    case ODIN3_AST_TASK_CALL:
        return reject_readmem(sc, node);
    default:
        return false;
    }
}

/* --- the IGNORE rows -------------------------------------------------------------------------- */

#define IGNORE(e1, node, text) odin3_elab1_warn_once((e1), (node), text)

/* A STRENGTH or DELAY slot of a declaration, assignment or gate. */
static void ignore_net(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    odin3_ast_id strength = kid(ast, node, D_STRENGTH_SLOT);
    odin3_ast_id delay = kid(ast, node, D_DELAY_SLOT_NET);
    uint32_t flags = odin3_ast_flags(ast, node);
    if (strength.v != 0) {
        if (odin3_ast_sub(ast, strength) == ODIN3_AST_STRENGTH_CHARGE) {
            IGNORE(e1, strength, "charge strength is ignored");
        } else {
            IGNORE(e1, strength, "drive strength is ignored");
        }
    }
    if (delay.v != 0) {
        IGNORE(e1, delay, "net delay is ignored");
    }
    if ((flags & ODIN3_AST_F_NET_DECL_VECTORED) != 0) {
        IGNORE(e1, node, "'vectored' is ignored");
    }
    if ((flags & ODIN3_AST_F_NET_DECL_SCALARED) != 0) {
        IGNORE(e1, node, "'scalared' is ignored");
    }
}

static void ignore_cont(odin3_elab1 *e1, odin3_ast_id node) {
    odin3_ast_id strength = kid(e1->parsed, node, D_STRENGTH_SLOT);
    odin3_ast_id delay = kid(e1->parsed, node, D_DELAY_SLOT_CONT);
    if (strength.v != 0) {
        IGNORE(e1, strength, "drive strength on a continuous assignment is ignored");
    }
    if (delay.v != 0) {
        IGNORE(e1, delay, "delay on a continuous assignment is ignored");
    }
}

static void ignore_gate(odin3_elab1 *e1, odin3_ast_id node) {
    odin3_ast_id strength = kid(e1->parsed, node, D_STRENGTH_SLOT);
    odin3_ast_id delay = kid(e1->parsed, node, D_DELAY_SLOT_CONT);
    if (strength.v != 0) {
        IGNORE(e1, strength, "gate drive strength is ignored");
    }
    if (delay.v != 0) {
        IGNORE(e1, delay, "gate delay is ignored");
    }
}

static void ignore_specparam(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    odin3_ast_span decls = odin3_ast_children(ast, node);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_PARAM_DECL); i < decls.n; i++) {
        if (decls.ids[i].v != 0) { /* the leading slot (RANGE?) is not a declarator */
            odin3_elab1_warn_once_at(e1, decls.ids[i], node, "specparam '%s' is ignored",
                                     odin3_ast_name_str(ast, decls.ids[i]));
        }
    }
}

/* T11: a delay in front of a statement or inside an assignment. */
static void ignore_delay(odin3_elab1 *e1, odin3_ast_id node, uint32_t idx) {
    odin3_ast_id ctl = kid(e1->parsed, node, idx);
    if (is_kind(e1->parsed, ctl, ODIN3_AST_DELAY)) {
        IGNORE(e1, ctl, "delay is ignored");
    }
}

static void ignore_other(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_DIRECTIVE:
        if (!odin3_elab1_directive_handled(e1, node)) {
            odin3_elab1_warn_once(e1, node, "`%s is ignored", odin3_ast_sub_name(ast, node));
        }
        break;
    case ODIN3_AST_SPECIFY_BLOCK:
        IGNORE(e1, node, "specify block is ignored");
        break;
    case ODIN3_AST_TASK_CALL:
        if ((odin3_ast_flags(ast, node) & ODIN3_AST_F_TASK_CALL_SYSTEM) != 0 &&
            !read_task(odin3_ast_name_str(ast, node))) {
            odin3_elab1_warn_once(e1, node, "system task '%s' is ignored",
                                  odin3_ast_name_str(ast, node));
        }
        break;
    default:
        break;
    }
}

odin3_status odin3_elab1_ignore(odin3_elab1 *e1, odin3_ast_id node) {
    const odin3_ast *ast = e1->parsed;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_NET_DECL:
        ignore_net(e1, node);
        break;
    case ODIN3_AST_CONT_ASSIGN:
        ignore_cont(e1, node);
        break;
    case ODIN3_AST_GATE_DECL:
        ignore_gate(e1, node);
        break;
    case ODIN3_AST_PARAM_DECL:
        if (odin3_ast_sub(ast, node) == ODIN3_AST_PARAM_DECL_SPECPARAM) {
            ignore_specparam(e1, node);
        }
        break;
    case ODIN3_AST_TIMING_STMT:
        ignore_delay(e1, node, 0);
        break;
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        ignore_delay(e1, node, D_CONTROL_SLOT);
        break;
    default:
        ignore_other(e1, node);
        break;
    }
    return odin3_elab1_status(e1);
}

void odin3_elab1_ignore_directive(odin3_elab1 *e1, odin3_ast_id directive) {
    (void)odin3_elab1_ignore(e1, directive);
}

/* --- the scan --------------------------------------------------------------------------------- */

/* Whether the walk goes into node's children: not into what is ignored, nor the DELAY and
 * STRENGTH slots (their parents warned for them). */
static bool descends(const diag_scan *sc, odin3_ast_id node) {
    const odin3_ast *ast = sc->ast;
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_DELAY:
    case ODIN3_AST_STRENGTH:
    case ODIN3_AST_SPECIFY_BLOCK:
    case ODIN3_AST_DIRECTIVE:
        return false;
    case ODIN3_AST_PARAM_DECL:
        return odin3_ast_sub(ast, node) != ODIN3_AST_PARAM_DECL_SPECPARAM;
    case ODIN3_AST_TASK_CALL:
        return (odin3_ast_flags(ast, node) & ODIN3_AST_F_TASK_CALL_SYSTEM) == 0 ||
               read_task(odin3_ast_name_str(ast, node));
    default:
        return true;
    }
}

/* Enter node: set the context it owns, report its rows. */
static void enter(diag_scan *sc, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(sc->ast, node);
    if (kind == ODIN3_AST_FUNCTION_DECL) {
        sc->fn_name = odin3_ast_name_str(sc->ast, node);
    } else if (kind == ODIN3_AST_INITIAL) {
        sc->initial = true;
    } else if (kind == ODIN3_AST_ALWAYS) {
        odin3_ast_id first = kid(sc->ast, node, 0);
        sc->top = is_kind(sc->ast, first, ODIN3_AST_TIMING_STMT) ? first : (odin3_ast_id){0};
    }
    (void)reject_free(sc->e1, node);
    if (!reject_ctx(sc, node)) { /* a control rejected in a function is not also ignored */
        (void)odin3_elab1_ignore(sc->e1, node);
    }
}

static void leave(diag_scan *sc, odin3_ast_id node) {
    odin3_ast_kind kind = odin3_ast_kind_of(sc->ast, node);
    if (kind == ODIN3_AST_FUNCTION_DECL) {
        sc->fn_name = NULL;
    } else if (kind == ODIN3_AST_INITIAL) {
        sc->initial = false;
    } else if (kind == ODIN3_AST_ALWAYS) {
        sc->top = (odin3_ast_id){0};
    }
}

static odin3_status push(diag_scan *sc, odin3_ast_id node) {
    diag_frame *frame = odin3_vec_push(&sc->frames);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (diag_frame){node.v, 0};
    enter(sc, node);
    if (!descends(sc, node)) {
        leave(sc, node);
        odin3_vec_pop(&sc->frames);
    }
    return ODIN3_OK;
}

static bool running(const diag_scan *sc) {
    return sc->frames.len > 0 && !sc->e1->stopped && sc->e1->fatal == ODIN3_OK;
}

static odin3_status step(diag_scan *sc) {
    diag_frame *top = odin3_vec_at(&sc->frames, sc->frames.len - 1);
    odin3_ast_id node = {top->node};
    odin3_ast_span kids = odin3_ast_children(sc->ast, node);
    if (top->next >= kids.n) {
        leave(sc, node);
        odin3_vec_pop(&sc->frames);
        return ODIN3_OK;
    }
    odin3_ast_id child = kids.ids[top->next++];
    return child.v != 0 ? push(sc, child) : ODIN3_OK;
}

odin3_status odin3_elab1_scan(odin3_elab1 *e1, uint32_t def, bool *failed) {
    odin3_elab1_defx *ext = odin3_vec_at(&e1->defx, def);
    if (ext->scanned) {
        *failed = ext->scan_failed;
        return ODIN3_OK;
    }
    ext->scanned = true;
    diag_scan sc = {.e1 = e1, .ast = e1->parsed};
    odin3_vec_init(&sc.frames, sizeof(diag_frame));
    uint32_t before = e1->errors;
    odin3_status st = push(&sc, odin3_elab1_def(e1, def)->node);
    while (st == ODIN3_OK && running(&sc)) {
        st = step(&sc);
    }
    odin3_vec_free(&sc.frames);
    ext = odin3_vec_at(&e1->defx, def);
    ext->scan_failed = e1->errors != before || e1->stopped;
    *failed = ext->scan_failed;
    return st;
}
