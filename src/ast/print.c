/*
 * print.c — the Verilog printer's engine (§11): an explicit task stack that kind handlers
 * (print_expr.c, print_stmt.c) fill in print order, plus the shared text helpers.
 */
#include "ast/print.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "backends/verilog/writer.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PRINTABLE_FIRST = 0x20, /* ' ' */
    PRINTABLE_LAST = 0x7e,  /* '~' */
    OCTAL_ESCAPE = 4,       /* "\ooo" */
    INDENT_BLANKS = 2,
};

/* --- handler output -------------------------------------------------------------------------- */

static void push_seq(odin3_printer *pr, odin3_pr_task task) {
    if (pr->st != ODIN3_OK) {
        return;
    }
    odin3_pr_task *slot = odin3_vec_push(&pr->seq);
    if (slot == NULL) {
        pr->st = ODIN3_ERR_NO_MEMORY;
        return;
    }
    *slot = task;
}

void odin3_pr_text(odin3_printer *pr, const char *text) {
    push_seq(pr, (odin3_pr_task){.text = text, .op = ODIN3_PR_OP_TEXT});
}

void odin3_pr_node(odin3_printer *pr, odin3_ast_id node, odin3_pr_mode mode, uint32_t indent) {
    if (node.v != 0) {
        push_seq(pr, (odin3_pr_task){.node = node.v,
                                     .indent = indent,
                                     .op = ODIN3_PR_OP_NODE,
                                     .mode = (uint8_t)mode});
    }
}

void odin3_pr_name(odin3_printer *pr, odin3_ast_id node) {
    push_seq(pr, (odin3_pr_task){.node = node.v, .op = ODIN3_PR_OP_NAME});
}

void odin3_pr_raw(odin3_printer *pr, odin3_ast_id node) {
    push_seq(pr, (odin3_pr_task){.node = node.v, .op = ODIN3_PR_OP_RAW});
}

void odin3_pr_indent(odin3_printer *pr, uint32_t indent) {
    if (indent > 0) {
        push_seq(pr, (odin3_pr_task){.indent = indent, .op = ODIN3_PR_OP_INDENT});
    }
}

void odin3_pr_leaf(odin3_printer *pr, odin3_ast_id node) {
    push_seq(pr, (odin3_pr_task){.node = node.v, .op = ODIN3_PR_OP_LEAF});
}

void odin3_pr_attrs(odin3_printer *pr, odin3_ast_id node, bool skip_meta) {
    odin3_ast_span attrs = odin3_ast_attrs(pr->ast, node);
    bool first = true;
    for (uint32_t i = 0; i < attrs.n; i++) {
        bool meta = (odin3_ast_flags(pr->ast, attrs.ids[i]) & ODIN3_AST_F_ATTR_METACOMMENT) != 0;
        if (meta && skip_meta) {
            continue;
        }
        odin3_pr_text(pr, first ? "(* " : ", ");
        odin3_pr_node(pr, attrs.ids[i], ODIN3_PR_LISTED, 0);
        first = false;
    }
    if (!first) {
        odin3_pr_text(pr, " *) ");
    }
}

void odin3_pr_list(odin3_printer *pr, odin3_pr_mode mode, odin3_ast_id node, uint32_t start) {
    odin3_ast_span kids = odin3_ast_children(pr->ast, node);
    for (uint32_t i = start; i < kids.n; i++) {
        if (i > start) {
            odin3_pr_text(pr, ", ");
        }
        odin3_pr_node(pr, kids.ids[i], mode, 0);
    }
}

/* --- text helpers ---------------------------------------------------------------------------- */

static bool plain_byte(uint8_t byte) {
    return byte >= PRINTABLE_FIRST && byte <= PRINTABLE_LAST && byte != '\\' && byte != '"';
}

static odin3_status append_escape(odin3_strbuf *out, uint8_t byte) {
    switch (byte) {
    case '\n':
        return odin3_strbuf_append(out, odin3_bytes_cstr("\\n"));
    case '\t':
        return odin3_strbuf_append(out, odin3_bytes_cstr("\\t"));
    case '\\':
        return odin3_strbuf_append(out, odin3_bytes_cstr("\\\\"));
    case '"':
        return odin3_strbuf_append(out, odin3_bytes_cstr("\\\""));
    default:
        return odin3_strbuf_appendf(out, "\\%03o", (unsigned)byte);
    }
}

odin3_status odin3_pr_append_cstring(odin3_strbuf *out, odin3_bytes bytes) {
    const uint8_t *text = bytes.ptr;
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    size_t pending = 0; /* start of the pending run of plain bytes */
    for (size_t i = 0; i < bytes.len && st == ODIN3_OK; i++) {
        if (plain_byte(text[i])) {
            continue;
        }
        st = odin3_strbuf_append(out, (odin3_bytes){text + pending, i - pending});
        st = st == ODIN3_OK ? append_escape(out, text[i]) : st;
        pending = i + 1;
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, (odin3_bytes){text + pending, bytes.len - pending});
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"")) : st;
}

odin3_bytes odin3_pr_name_bytes(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_strtab *tab = odin3_design_strtab(odin3_ast_design(ast));
    uint32_t name = odin3_ast_name(ast, node);
    const char *str = odin3_strtab_get(tab, name);
    return (odin3_bytes){(const uint8_t *)(str != NULL ? str : ""), odin3_strtab_len(tab, name)};
}

odin3_ast_id odin3_pr_self(const odin3_pr_task *task) {
    return (odin3_ast_id){task->node};
}

odin3_ast_id odin3_pr_kid(const odin3_printer *pr, const odin3_pr_task *task, uint32_t idx) {
    return odin3_ast_child(pr->ast, odin3_pr_self(task), idx);
}

uint32_t odin3_pr_flags(const odin3_printer *pr, const odin3_pr_task *task) {
    return odin3_ast_flags(pr->ast, odin3_pr_self(task));
}

odin3_ast_kind odin3_pr_kind(const odin3_printer *pr, const odin3_pr_task *task) {
    return odin3_ast_kind_of(pr->ast, odin3_pr_self(task));
}

/*
 * node's name as an identifier. A name Verilog cannot spell (empty, a blank, a backtick, a control
 * or non-ASCII byte; no parse stores one) is written `\name ` anyway, with a warning: the output
 * then does not re-parse to the same tree.
 */
static odin3_status append_name(const odin3_ast *ast, odin3_ast_id node, odin3_strbuf *out) {
    odin3_bytes name = odin3_pr_name_bytes(ast, node);
    if (odin3_verilog_ident_kind(name) != ODIN3_VERILOG_UNWRITABLE) {
        return odin3_verilog_append_ident(out, name);
    }
    odin3_log(ODIN3_LOG_WARN,
              "odin3_ast_print: the name of node %" PRIu32
              " is not a writable Verilog identifier; written as stored",
              node.v);
    if (name.len == 0) {
        return ODIN3_OK;
    }
    return odin3_strbuf_appendf(out, "\\%.*s ", (int)name.len, (const char *)name.ptr);
}

static odin3_status append_leaf(const odin3_ast *ast, odin3_ast_id node, odin3_strbuf *out) {
    switch (odin3_ast_kind_of(ast, node)) {
    case ODIN3_AST_NUMBER:
        return odin3_pr_append_number(out, ast, node);
    case ODIN3_AST_REAL:
        return odin3_pr_append_real(out, odin3_ast_real(ast, node));
    case ODIN3_AST_STRING:
        return odin3_pr_append_cstring(out, odin3_pr_name_bytes(ast, node));
    default: /* the TEXT kinds */
        return odin3_strbuf_append(out, odin3_ast_text(ast, node));
    }
}

static odin3_status append_indent(odin3_strbuf *out, uint32_t indent) {
    indent = indent < ODIN3_PR_MAX_INDENT ? indent : ODIN3_PR_MAX_INDENT;
    return odin3_strbuf_appendf(out, "%*s", (int)(indent * INDENT_BLANKS), "");
}

/* --- the engine ------------------------------------------------------------------------------ */

static odin3_pr_handler handler_of(uint32_t kind) {
    odin3_pr_handler fn = odin3_pr_expr_handler(kind);
    return fn != NULL ? fn : odin3_pr_stmt_handler(kind);
}

/* Runs the node's handler and moves its output onto the stack, first part on top. */
static void expand(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_id node = {task->node};
    odin3_pr_handler fn = handler_of(odin3_ast_kind_of(pr->ast, node));
    if (fn == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_print: node %" PRIu32 " has an unknown kind",
                  task->node);
        pr->st = ODIN3_ERR_INVALID_ARG;
        return;
    }
    odin3_vec_clear(&pr->seq);
    fn(pr, task);
    if (pr->st != ODIN3_OK) {
        return;
    }
    if (odin3_vec_reserve(&pr->stack, pr->stack.len + pr->seq.len) != ODIN3_OK) {
        pr->st = ODIN3_ERR_NO_MEMORY;
        return;
    }
    const odin3_pr_task *parts = pr->seq.data;
    for (size_t i = pr->seq.len; i-- > 0;) {
        *(odin3_pr_task *)odin3_vec_push(&pr->stack) = parts[i];
    }
}

static void run(odin3_printer *pr, const odin3_pr_task *task) {
    odin3_ast_id node = {task->node};
    switch (task->op) {
    case ODIN3_PR_OP_NODE:
        expand(pr, task);
        return;
    case ODIN3_PR_OP_TEXT:
        pr->st = odin3_strbuf_append(pr->out, odin3_bytes_cstr(task->text));
        return;
    case ODIN3_PR_OP_NAME:
        pr->st = append_name(pr->ast, node, pr->out);
        return;
    case ODIN3_PR_OP_RAW:
        pr->st = odin3_strbuf_append(pr->out, odin3_pr_name_bytes(pr->ast, node));
        return;
    case ODIN3_PR_OP_INDENT:
        pr->st = append_indent(pr->out, task->indent);
        return;
    default:
        pr->st = append_leaf(pr->ast, node, pr->out);
        return;
    }
}

/* How a root prints: expressions bare, items and statements as lines, the rest as fragments. */
static odin3_pr_mode root_mode(odin3_ast_kind kind) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    if (odin3_ast_class_has(ODIN3_AST_CLS_E, info)) {
        return ODIN3_PR_EXPR;
    }
    bool line = odin3_ast_class_has(ODIN3_AST_CLS_S, info) ||
                odin3_ast_class_has(ODIN3_AST_CLS_I, info) ||
                odin3_ast_class_has(ODIN3_AST_CLS_U, info) || kind == ODIN3_AST_UNIT ||
                kind == ODIN3_AST_CASE_ITEM;
    return line ? ODIN3_PR_ITEM : ODIN3_PR_LISTED;
}

odin3_status odin3_ast_print(const odin3_ast *ast, odin3_ast_id root, odin3_strbuf *out) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, root);
    if (ast == NULL || out == NULL || kind == ODIN3_AST_NONE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_print: %s",
                  ast == NULL || out == NULL ? "NULL argument" : "root is 0 or not made");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_printer pr = {.ast = ast, .out = out, .st = ODIN3_OK};
    odin3_vec_init(&pr.stack, sizeof(odin3_pr_task));
    odin3_vec_init(&pr.seq, sizeof(odin3_pr_task));
    odin3_pr_task *first = odin3_vec_push(&pr.stack);
    if (first == NULL) {
        pr.st = ODIN3_ERR_NO_MEMORY;
    } else {
        *first = (odin3_pr_task){.node = root.v, .mode = (uint8_t)root_mode(kind)};
    }
    while (pr.st == ODIN3_OK && pr.stack.len > 0) {
        odin3_pr_task task = *(const odin3_pr_task *)odin3_vec_cat(&pr.stack, pr.stack.len - 1);
        odin3_vec_pop(&pr.stack);
        run(&pr, &task);
    }
    odin3_vec_free(&pr.stack);
    odin3_vec_free(&pr.seq);
    return pr.st;
}
