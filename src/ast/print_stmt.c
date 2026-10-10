/*
 * print_stmt.c — the printer's handlers for design units, module items, declarations, generate
 * constructs and statements (§11): each line at its indent, attributes before the item,
 * nested items and statements one level deeper.
 */
#include "ast/print.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>

/* An item's start: its indent on a fresh line, then its `(* … *)` group. */
static void open_item(odin3_printer *pr, const odin3_pr_task *task) {
    if (task->mode == ODIN3_PR_ITEM) {
        odin3_pr_indent(pr, task->indent);
    }
    odin3_ast_kind kind = odin3_pr_kind(pr, task);
    odin3_pr_attrs(pr, odin3_pr_self(task), kind == ODIN3_AST_CASE || kind == ODIN3_AST_GEN_CASE);
}

/* An item's end: `;` and the newline, unless it sits in a list or header. */
static void close_item(odin3_printer *pr, const odin3_pr_task *task) {
    if (task->mode != ODIN3_PR_LISTED) {
        odin3_pr_text(pr, ";\n");
    }
}

/* " " + child, when the optional child is present. */
static void spaced(odin3_printer *pr, odin3_ast_id child) {
    if (child.v != 0) {
        odin3_pr_text(pr, " ");
        odin3_pr_node(pr, child, ODIN3_PR_LISTED, 0);
    }
}

/* A sub-statement continuing the line (`;` when it is absent). */
static void inline_stmt(odin3_printer *pr, odin3_ast_id stmt, uint32_t indent) {
    if (stmt.v == 0) {
        odin3_pr_text(pr, ";\n");
    } else {
        odin3_pr_node(pr, stmt, ODIN3_PR_INLINE, indent);
    }
}

/* Children start.. of node, each an item on its own line at indent. */
static void items(odin3_printer *pr, uint32_t indent, odin3_ast_id node, uint32_t start) {
    uint32_t count = odin3_ast_nchild(pr->ast, node);
    for (uint32_t i = start; i < count; i++) {
        odin3_pr_node(pr, odin3_ast_child(pr->ast, node, i), ODIN3_PR_ITEM, indent);
    }
}

/* The closing keyword line: indent + word + newline. */
static void close_block(odin3_printer *pr, const odin3_pr_task *task, const char *word) {
    odin3_pr_indent(pr, task->indent);
    odin3_pr_text(pr, word);
}

/* The opening and closing words of the kinds that print one. */
static const char *const OPEN_WORD[ODIN3_AST_KIND_COUNT] = {
    [ODIN3_AST_GENERATE] = "generate", [ODIN3_AST_GEN_BLOCK] = "begin",
    [ODIN3_AST_SEQ_BLOCK] = "begin",   [ODIN3_AST_PAR_BLOCK] = "fork",
    [ODIN3_AST_INITIAL] = "initial",   [ODIN3_AST_FOREVER] = "forever",
    [ODIN3_AST_WHILE] = "while (",     [ODIN3_AST_REPEAT] = "repeat (",
    [ODIN3_AST_WAIT] = "wait (",       [ODIN3_AST_FUNCTION_DECL] = "function",
    [ODIN3_AST_TASK_DECL] = "task"};
static const char *const CLOSE_WORD[ODIN3_AST_KIND_COUNT] = {[ODIN3_AST_GENERATE] = "endgenerate\n",
                                                             [ODIN3_AST_GEN_BLOCK] = "end\n",
                                                             [ODIN3_AST_SEQ_BLOCK] = "end\n",
                                                             [ODIN3_AST_PAR_BLOCK] = "join\n",
                                                             [ODIN3_AST_FUNCTION_DECL] =
                                                                 "endfunction\n",
                                                             [ODIN3_AST_TASK_DECL] = "endtask\n"};

/* Each kind's own bit for the one flag its handler shares with another kind. */
static const uint16_t KIND_FLAG[ODIN3_AST_KIND_COUNT] = {
    [ODIN3_AST_VAR_DECL] = ODIN3_AST_F_VAR_DECL_SIGNED,
    [ODIN3_AST_PARAM_DECL] = ODIN3_AST_F_PARAM_DECL_SIGNED,
    [ODIN3_AST_FUNCTION_DECL] = ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC,
    [ODIN3_AST_TASK_DECL] = ODIN3_AST_F_TASK_DECL_AUTOMATIC};

static const char *dt_name(uint32_t flags, uint32_t mask, uint32_t shift) {
    static const char *const DT[] = {"", " reg", " integer", " real", " realtime", " time"};
    uint32_t dt = (flags & mask) >> shift;
    return dt < sizeof DT / sizeof DT[0] ? DT[dt] : "";
}

/* --- design units and modules ----------------------------------------------------------------- */

static void print_unit(odin3_printer *pr, const odin3_pr_task *task) {
    items(pr, task->indent, odin3_pr_self(task), 0);
}

static void print_directive(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    odin3_pr_text(pr, "`");
    odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
    if (odin3_ast_name(pr->ast, odin3_pr_self(task)) != 0) {
        odin3_pr_text(pr, " ");
        odin3_pr_raw(pr, odin3_pr_self(task));
    }
    odin3_pr_text(pr, task->mode == ODIN3_PR_LISTED ? "" : "\n");
}

static void print_text_item(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    odin3_pr_leaf(pr, odin3_pr_self(task));
    odin3_pr_text(pr, task->mode == ODIN3_PR_LISTED ? "" : "\n");
}

/* A module header list: `(`, one entry per line one level in, `)` back at indent. */
static void header_list(odin3_printer *pr, odin3_ast_id list, uint32_t indent) {
    uint32_t count = odin3_ast_nchild(pr->ast, list);
    odin3_pr_text(pr, "(");
    for (uint32_t i = 0; i < count; i++) {
        odin3_pr_text(pr, i > 0 ? ",\n" : "\n");
        odin3_pr_indent(pr, indent + 1);
        odin3_pr_node(pr, odin3_ast_child(pr->ast, list, i), ODIN3_PR_LISTED, indent + 1);
    }
    if (count > 0) {
        odin3_pr_text(pr, "\n");
        odin3_pr_indent(pr, indent);
    }
    odin3_pr_text(pr, ")");
}

static void print_module(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    bool macro = (odin3_pr_flags(pr, task) & ODIN3_AST_F_MODULE_MACROMODULE) != 0;
    odin3_pr_text(pr, macro ? "macromodule " : "module ");
    odin3_pr_name(pr, odin3_pr_self(task));
    if (odin3_pr_kid(pr, task, 0).v != 0) {
        odin3_pr_text(pr, " #");
        header_list(pr, odin3_pr_kid(pr, task, 0), task->indent);
    }
    if (odin3_pr_kid(pr, task, 1).v != 0) {
        odin3_pr_text(pr, " ");
        header_list(pr, odin3_pr_kid(pr, task, 1), task->indent);
    }
    odin3_pr_text(pr, ";\n");
    items(pr, task->indent + 1, odin3_pr_self(task), 2);
    close_block(pr, task, "endmodule\n");
}

/* --- declarations ----------------------------------------------------------------------------- */

static void print_port_decl(odin3_printer *pr, const odin3_pr_task *task) {
    const odin3_ast_kind_info *nets = odin3_ast_kind_info_of(ODIN3_AST_NET_DECL);
    uint32_t flags = odin3_pr_flags(pr, task);
    uint32_t net = (flags & ODIN3_AST_F_PORT_DECL_NET_MASK) >> ODIN3_AST_F_PORT_DECL_NET_SHIFT;
    open_item(pr, task);
    odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
    if (net != 0 && net <= nets->sub_count) {
        odin3_pr_text(pr, " ");
        odin3_pr_text(pr, nets->sub_names[net - 1]);
    }
    odin3_pr_text(pr,
                  dt_name(flags, ODIN3_AST_F_PORT_DECL_DT_MASK, ODIN3_AST_F_PORT_DECL_DT_SHIFT));
    odin3_pr_text(pr, (flags & ODIN3_AST_F_PORT_DECL_SIGNED) != 0 ? " signed" : "");
    spaced(pr, odin3_pr_kid(pr, task, 0));
    odin3_pr_text(pr, " ");
    odin3_pr_list(pr, ODIN3_PR_LISTED, odin3_pr_self(task), 1);
    close_item(pr, task);
}

static void print_net_decl(odin3_printer *pr, const odin3_pr_task *task) {
    uint32_t flags = odin3_pr_flags(pr, task);
    open_item(pr, task);
    odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
    spaced(pr, odin3_pr_kid(pr, task, 0));
    odin3_pr_text(pr, (flags & ODIN3_AST_F_NET_DECL_VECTORED) != 0 ? " vectored" : "");
    odin3_pr_text(pr, (flags & ODIN3_AST_F_NET_DECL_SCALARED) != 0 ? " scalared" : "");
    odin3_pr_text(pr, (flags & ODIN3_AST_F_NET_DECL_SIGNED) != 0 ? " signed" : "");
    spaced(pr, odin3_pr_kid(pr, task, 1));
    spaced(pr, odin3_pr_kid(pr, task, 2));
    odin3_pr_text(pr, " ");
    odin3_pr_list(pr, ODIN3_PR_LISTED, odin3_pr_self(task), 3);
    close_item(pr, task);
}

/* VAR_DECL and PARAM_DECL: kind [type] [signed] [range] declarators. */
static void print_var_or_param(odin3_printer *pr, const odin3_pr_task *task) {
    uint32_t flags = odin3_pr_flags(pr, task);
    bool param = odin3_pr_kind(pr, task) == ODIN3_AST_PARAM_DECL;
    open_item(pr, task);
    odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
    if (param) {
        odin3_pr_text(
            pr, dt_name(flags, ODIN3_AST_F_PARAM_DECL_DT_MASK, ODIN3_AST_F_PARAM_DECL_DT_SHIFT));
    }
    odin3_pr_text(pr, (flags & KIND_FLAG[odin3_pr_kind(pr, task)]) != 0 ? " signed" : "");
    spaced(pr, odin3_pr_kid(pr, task, 0));
    odin3_pr_text(pr, " ");
    odin3_pr_list(pr, ODIN3_PR_LISTED, odin3_pr_self(task), 1);
    close_item(pr, task);
}

/* GENVAR_DECL, EVENT_DECL, DEFPARAM, DISABLE, EVENT_TRIGGER: keyword + list. */
static void print_keyword_list(odin3_printer *pr, const odin3_pr_task *task) {
    static const char *const SEPARATOR[ODIN3_AST_KIND_COUNT] = {[ODIN3_AST_GENVAR_DECL] = ", ",
                                                                [ODIN3_AST_EVENT_DECL] = ", ",
                                                                [ODIN3_AST_DEFPARAM] = " = ",
                                                                [ODIN3_AST_DISABLE] = "",
                                                                [ODIN3_AST_EVENT_TRIGGER] = ""};
    static const char *const KEYWORD[ODIN3_AST_KIND_COUNT] = {[ODIN3_AST_GENVAR_DECL] = "genvar ",
                                                              [ODIN3_AST_EVENT_DECL] = "event ",
                                                              [ODIN3_AST_DEFPARAM] = "defparam ",
                                                              [ODIN3_AST_DISABLE] = "disable ",
                                                              [ODIN3_AST_EVENT_TRIGGER] = "-> "};
    odin3_ast_kind kind = odin3_pr_kind(pr, task);
    uint32_t count = odin3_ast_nchild(pr->ast, odin3_pr_self(task));
    open_item(pr, task);
    odin3_pr_text(pr, KEYWORD[kind]);
    for (uint32_t i = 0; i < count; i++) {
        odin3_pr_text(pr, i > 0 ? SEPARATOR[kind] : "");
        odin3_pr_node(pr, odin3_pr_kid(pr, task, i), ODIN3_PR_LISTED, 0);
    }
    close_item(pr, task);
}

/* CONT_ASSIGN, GATE_DECL, INSTANTIATION: head [strength] [delay | #(params)] list. */
static void print_instances(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_kind kind = odin3_pr_kind(pr, task);
    open_item(pr, task);
    uint32_t first = 2;
    if (kind == ODIN3_AST_INSTANTIATION) {
        odin3_pr_name(pr, odin3_pr_self(task));
        if (odin3_pr_kid(pr, task, 0).v != 0) {
            odin3_pr_text(pr, " #(");
            odin3_pr_list(pr, ODIN3_PR_LISTED, odin3_pr_kid(pr, task, 0), 0);
            odin3_pr_text(pr, ")");
        }
        first = 1;
    } else {
        odin3_pr_text(pr, kind == ODIN3_AST_CONT_ASSIGN
                              ? "assign"
                              : odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
        spaced(pr, odin3_pr_kid(pr, task, 0));
        spaced(pr, odin3_pr_kid(pr, task, 1));
    }
    odin3_pr_text(pr, " ");
    odin3_pr_list(pr, ODIN3_PR_LISTED, odin3_pr_self(task), first);
    close_item(pr, task);
}

static void print_instance(odin3_printer *pr, const odin3_pr_task *task) {
    if (odin3_ast_name(pr->ast, odin3_pr_self(task)) != 0) {
        odin3_pr_name(pr, odin3_pr_self(task));
        odin3_pr_text(pr, " ");
    }
    if (odin3_pr_kid(pr, task, 0).v != 0) {
        odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_LISTED, 0);
        odin3_pr_text(pr, " ");
    }
    odin3_pr_text(pr, "(");
    odin3_pr_list(pr, ODIN3_PR_LISTED, odin3_pr_self(task), 1);
    odin3_pr_text(pr, ")");
}

/* --- functions and tasks ---------------------------------------------------------------------- */

/* The ports: `(…)` in the header when declared there, else items after the header's `;`. */
static bool header_ports(const odin3_printer *pr, odin3_ast_id ports) {
    odin3_ast_id first = odin3_ast_child(pr->ast, ports, 0);
    return odin3_ast_kind_of(pr->ast, first) == ODIN3_AST_PORT_DECL &&
           (odin3_ast_flags(pr->ast, first) & ODIN3_AST_F_PORT_DECL_IN_HEADER) != 0;
}

/* FUNCTION_DECL (R? ports locals body) and TASK_DECL (ports locals body). */
static void print_subroutine(odin3_printer *pr, const odin3_pr_task *task) {
    bool fn = odin3_pr_kind(pr, task) == ODIN3_AST_FUNCTION_DECL;
    uint32_t base = fn ? 1 : 0; /* index of the ports LIST */
    uint32_t flags = odin3_pr_flags(pr, task);
    open_item(pr, task);
    odin3_pr_text(pr, OPEN_WORD[odin3_pr_kind(pr, task)]);
    odin3_pr_text(pr, (flags & KIND_FLAG[odin3_pr_kind(pr, task)]) != 0 ? " automatic" : "");
    if (fn) {
        odin3_pr_text(pr, dt_name(flags, ODIN3_AST_F_FUNCTION_DECL_DT_MASK,
                                  ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT));
        odin3_pr_text(pr, (flags & ODIN3_AST_F_FUNCTION_DECL_SIGNED) != 0 ? " signed" : "");
        spaced(pr, odin3_pr_kid(pr, task, 0));
    }
    odin3_pr_text(pr, " ");
    odin3_pr_name(pr, odin3_pr_self(task));
    odin3_ast_id ports = odin3_pr_kid(pr, task, base);
    bool in_header = header_ports(pr, ports);
    if (in_header) {
        odin3_pr_text(pr, "(");
        odin3_pr_list(pr, ODIN3_PR_LISTED, ports, 0);
        odin3_pr_text(pr, ")");
    }
    odin3_pr_text(pr, ";\n");
    if (!in_header) {
        items(pr, task->indent + 1, ports, 0);
    }
    items(pr, task->indent + 1, odin3_pr_kid(pr, task, base + 1), 0);
    odin3_pr_node(pr, odin3_pr_kid(pr, task, base + 2), ODIN3_PR_ITEM, task->indent + 1);
    close_block(pr, task, CLOSE_WORD[odin3_pr_kind(pr, task)]);
}

/* --- blocks ----------------------------------------------------------------------------------- */

/* GENERATE, GEN_BLOCK, SEQ_BLOCK, PAR_BLOCK: open word [: name[index]], items, close word. */
static void print_block(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_kind kind = odin3_pr_kind(pr, task);
    bool par = kind == ODIN3_AST_PAR_BLOCK;
    bool gen = kind == ODIN3_AST_GENERATE;
    open_item(pr, task);
    odin3_pr_text(pr, OPEN_WORD[kind]);
    if (odin3_ast_name(pr->ast, odin3_pr_self(task)) != 0) {
        odin3_pr_text(pr, " : ");
        odin3_pr_name(pr, odin3_pr_self(task));
    }
    if (kind == ODIN3_AST_GEN_BLOCK && odin3_pr_kid(pr, task, 0).v != 0) {
        odin3_pr_text(pr, "[");
        odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_EXPR, 0);
        odin3_pr_text(pr, "]");
    }
    odin3_pr_text(pr, "\n");
    if (kind == ODIN3_AST_SEQ_BLOCK || par) {
        items(pr, task->indent + 1, odin3_pr_kid(pr, task, 0), 0); /* the locals */
    }
    items(pr, task->indent + 1, odin3_pr_self(task), gen ? 0 : 1);
    close_block(pr, task, CLOSE_WORD[kind]);
}

/* ALWAYS, INITIAL, FOREVER: keyword + statement. */
static void print_keyword_stmt(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_kind kind = odin3_pr_kind(pr, task);
    open_item(pr, task);
    odin3_pr_text(pr, kind == ODIN3_AST_ALWAYS ? odin3_ast_sub_name(pr->ast, odin3_pr_self(task))
                                               : OPEN_WORD[kind]);
    odin3_pr_text(pr, " ");
    inline_stmt(pr, odin3_pr_kid(pr, task, 0), task->indent);
}

/* WHILE, REPEAT, WAIT: keyword (e) statement (`;` for a WAIT without one). */
static void print_cond_stmt(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_kind kind = odin3_pr_kind(pr, task);
    open_item(pr, task);
    odin3_pr_text(pr, OPEN_WORD[kind]);
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, odin3_pr_kid(pr, task, 1).v != 0 ? ") " : ")");
    inline_stmt(pr, odin3_pr_kid(pr, task, 1), task->indent);
}

static void print_timing_stmt(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_LISTED, 0);
    odin3_pr_text(pr, odin3_pr_kid(pr, task, 1).v != 0 ? " " : "");
    inline_stmt(pr, odin3_pr_kid(pr, task, 1), task->indent);
}

/* FOR and GEN_FOR: for (init; cond; step) body. */
static void print_for(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    odin3_pr_text(pr, "for (");
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_LISTED, 0);
    odin3_pr_text(pr, "; ");
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 1), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, "; ");
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 2), ODIN3_PR_LISTED, 0);
    odin3_pr_text(pr, ") ");
    inline_stmt(pr, odin3_pr_kid(pr, task, 3), task->indent);
}

/* True when stmt ends in an if without an else, which would capture a following `else`. */
static bool ends_open_if(const odin3_ast *ast, odin3_ast_id stmt) {
    static const uint8_t TAIL_SLOT[ODIN3_AST_KIND_COUNT] = {
        [ODIN3_AST_IF] = 3,    [ODIN3_AST_GEN_IF] = 3, [ODIN3_AST_TIMING_STMT] = 2,
        [ODIN3_AST_WAIT] = 2,  [ODIN3_AST_FOR] = 4,    [ODIN3_AST_GEN_FOR] = 4,
        [ODIN3_AST_WHILE] = 2, [ODIN3_AST_REPEAT] = 2, [ODIN3_AST_FOREVER] = 1};
    while (stmt.v != 0) { /* the last slot (1-based) holds the trailing statement */
        odin3_ast_kind kind = odin3_ast_kind_of(ast, stmt);
        if (kind >= ODIN3_AST_KIND_COUNT || TAIL_SLOT[kind] == 0) {
            return false;
        }
        odin3_ast_id last = odin3_ast_child(ast, stmt, TAIL_SLOT[kind] - 1U);
        if (last.v == 0 && (kind == ODIN3_AST_IF || kind == ODIN3_AST_GEN_IF)) {
            return true;
        }
        stmt = last;
    }
    return false;
}

/* IF and GEN_IF; a then-branch that would capture the else is wrapped in begin/end. */
static void print_if(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_id then = odin3_pr_kid(pr, task, 1);
    odin3_ast_id other = odin3_pr_kid(pr, task, 2);
    open_item(pr, task);
    odin3_pr_text(pr, "if (");
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, ") ");
    if (other.v != 0 && ends_open_if(pr->ast, then)) {
        odin3_pr_text(pr, "begin\n");
        odin3_pr_node(pr, then, ODIN3_PR_ITEM, task->indent + 1);
        close_block(pr, task, "end\n");
    } else {
        inline_stmt(pr, then, task->indent);
    }
    if (other.v != 0) {
        close_block(pr, task, "else ");
        inline_stmt(pr, other, task->indent);
    }
}

/* The METACOMMENT attributes as `// synopsys k …` after a case header. */
static void metacomments(odin3_printer *pr, odin3_ast_id node) {
    odin3_ast_span attrs = odin3_ast_attrs(pr->ast, node);
    bool first = true;
    for (uint32_t i = 0; i < attrs.n; i++) {
        if ((odin3_ast_flags(pr->ast, attrs.ids[i]) & ODIN3_AST_F_ATTR_METACOMMENT) != 0) {
            odin3_pr_text(pr, first ? " // synopsys " : " ");
            odin3_pr_raw(pr, attrs.ids[i]);
            first = false;
        }
    }
}

/* CASE and GEN_CASE. */
static void print_case(odin3_printer *pr, const odin3_pr_task *task) {
    bool gen = odin3_pr_kind(pr, task) == ODIN3_AST_GEN_CASE;
    open_item(pr, task);
    odin3_pr_text(pr, gen ? "case" : odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
    odin3_pr_text(pr, " (");
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, ")");
    metacomments(pr, odin3_pr_self(task));
    odin3_pr_text(pr, "\n");
    items(pr, task->indent + 1, odin3_pr_self(task), 1);
    close_block(pr, task, "endcase\n");
}

/* A CASE_ITEM: its body is slot 0 but prints after the match expressions. */
static void print_case_item(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    if ((odin3_pr_flags(pr, task) & ODIN3_AST_F_CASE_ITEM_DEFAULT) != 0) {
        odin3_pr_text(pr, "default");
    } else {
        odin3_pr_list(pr, ODIN3_PR_EXPR, odin3_pr_self(task), 1);
    }
    odin3_pr_text(pr, ": ");
    inline_stmt(pr, odin3_pr_kid(pr, task, 0), task->indent);
}

/* --- simple statements ------------------------------------------------------------------------ */

static void print_assign(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    odin3_pr_text(pr, odin3_pr_kind(pr, task) == ODIN3_AST_NONBLOCKING_ASSIGN ? " <= " : " = ");
    if (odin3_pr_kid(pr, task, 1).v != 0) {
        odin3_pr_node(pr, odin3_pr_kid(pr, task, 1), ODIN3_PR_LISTED, 0);
        odin3_pr_text(pr, " ");
    }
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 2), ODIN3_PR_EXPR, 0);
    close_item(pr, task);
}

static void print_proc_assign(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    odin3_pr_text(pr, odin3_ast_sub_name(pr->ast, odin3_pr_self(task)));
    odin3_pr_text(pr, " ");
    odin3_pr_node(pr, odin3_pr_kid(pr, task, 0), ODIN3_PR_EXPR, 0);
    if (odin3_pr_kid(pr, task, 1).v != 0) {
        odin3_pr_text(pr, " = ");
        odin3_pr_node(pr, odin3_pr_kid(pr, task, 1), ODIN3_PR_EXPR, 0);
    }
    close_item(pr, task);
}

static void print_task_call(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    if ((odin3_pr_flags(pr, task) & (ODIN3_AST_F_TASK_CALL_SYSTEM | ODIN3_AST_F_TASK_CALL_HIER)) !=
        0) {
        odin3_pr_raw(pr, odin3_pr_self(task));
    } else {
        odin3_pr_name(pr, odin3_pr_self(task));
    }
    if (odin3_ast_nchild(pr->ast, odin3_pr_self(task)) > 0) {
        odin3_pr_text(pr, "(");
        odin3_pr_list(pr, ODIN3_PR_EXPR, odin3_pr_self(task), 0);
        odin3_pr_text(pr, ")");
    }
    close_item(pr, task);
}

static void print_null(odin3_printer *pr, const odin3_pr_task *task) {
    open_item(pr, task);
    close_item(pr, task);
}

odin3_pr_handler odin3_pr_stmt_handler(uint32_t kind) {
    static const odin3_pr_handler HANDLERS[ODIN3_AST_KIND_COUNT] = {
        [ODIN3_AST_UNIT] = print_unit,
        [ODIN3_AST_DIRECTIVE] = print_directive,
        [ODIN3_AST_MODULE] = print_module,
        [ODIN3_AST_UDP_DECL] = print_text_item,
        [ODIN3_AST_CONFIG_DECL] = print_text_item,
        [ODIN3_AST_PORT_DECL] = print_port_decl,
        [ODIN3_AST_NET_DECL] = print_net_decl,
        [ODIN3_AST_VAR_DECL] = print_var_or_param,
        [ODIN3_AST_PARAM_DECL] = print_var_or_param,
        [ODIN3_AST_DEFPARAM] = print_keyword_list,
        [ODIN3_AST_GENVAR_DECL] = print_keyword_list,
        [ODIN3_AST_EVENT_DECL] = print_keyword_list,
        [ODIN3_AST_CONT_ASSIGN] = print_instances,
        [ODIN3_AST_ALWAYS] = print_keyword_stmt,
        [ODIN3_AST_INITIAL] = print_keyword_stmt,
        [ODIN3_AST_INSTANTIATION] = print_instances,
        [ODIN3_AST_INSTANCE] = print_instance,
        [ODIN3_AST_GATE_DECL] = print_instances,
        [ODIN3_AST_FUNCTION_DECL] = print_subroutine,
        [ODIN3_AST_TASK_DECL] = print_subroutine,
        [ODIN3_AST_SPECIFY_BLOCK] = print_text_item,
        [ODIN3_AST_GENERATE] = print_block,
        [ODIN3_AST_GEN_FOR] = print_for,
        [ODIN3_AST_GEN_IF] = print_if,
        [ODIN3_AST_GEN_CASE] = print_case,
        [ODIN3_AST_GEN_BLOCK] = print_block,
        [ODIN3_AST_SEQ_BLOCK] = print_block,
        [ODIN3_AST_PAR_BLOCK] = print_block,
        [ODIN3_AST_BLOCKING_ASSIGN] = print_assign,
        [ODIN3_AST_NONBLOCKING_ASSIGN] = print_assign,
        [ODIN3_AST_PROC_CONT_ASSIGN] = print_proc_assign,
        [ODIN3_AST_IF] = print_if,
        [ODIN3_AST_CASE] = print_case,
        [ODIN3_AST_CASE_ITEM] = print_case_item,
        [ODIN3_AST_FOR] = print_for,
        [ODIN3_AST_WHILE] = print_cond_stmt,
        [ODIN3_AST_REPEAT] = print_cond_stmt,
        [ODIN3_AST_FOREVER] = print_keyword_stmt,
        [ODIN3_AST_TIMING_STMT] = print_timing_stmt,
        [ODIN3_AST_WAIT] = print_cond_stmt,
        [ODIN3_AST_DISABLE] = print_keyword_list,
        [ODIN3_AST_EVENT_TRIGGER] = print_keyword_list,
        [ODIN3_AST_TASK_CALL] = print_task_call,
        [ODIN3_AST_NULL_STMT] = print_null,
    };
    return kind < ODIN3_AST_KIND_COUNT ? HANDLERS[kind] : NULL;
}
