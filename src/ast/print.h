/*
 * print.h — the AST's text forms (spec §6, §11): the Verilog printer, the s-expression dump and
 * structural equality, the parse–print–parse oracle 2C tests every construct with. All three
 * are iterative (explicit stacks), never recursive.
 */
#ifndef ODIN3_AST_PRINT_H
#define ODIN3_AST_PRINT_H

#include "ast/ast.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Appends the subtree of root as Verilog-2005 (§11) to out. Layout: the 1F writer's (2-space
 * indent per level; a module's ports one per line, `module m (\n  input [3:0] a,\n  output y\n);`);
 * an item or statement ends with a newline, an expression root does not; indentation stops
 * growing at ODIN3_PR_MAX_INDENT levels, so a pathologically deep tree stays linear in size. Every
 * operand that is a UNARY, BINARY or TERNARY is parenthesised, the top expression is not. Names go
 * through odin3_verilog_append_ident (`\wire `, `\a+b `); a `$`-name or a dotted HIER name is
 * written as stored; a GEN_BLOCK's index slot prints as `g[3]`. NUMBER: `[width]'[s]<base><digits>`
 * in lower case from the stored bits (a plain decimal bare; an all-z digit `?` when has_question; a
 * digit mixing x/z with 0/1, or a decimal mixing them, prints the literal in binary). REAL: `%.17g`
 * (with `.0` added when that reads as an integer). STRING: re-escaped (`\n \t \\ \"`, other bytes
 * outside printable ASCII `\ooo`). TEXT payloads verbatim. Attributes `(* k *)` / `(* k = v *)`
 * before an item or statement and after the operator of a UNARY/BINARY/TERNARY; METACOMMENT
 * attributes of a CASE/GEN_CASE as `// synopsys <keys>` after its `case (e)` header. Comments
 * are omitted. A then-branch that would capture the else (`if (a) if (b) x; else y;` from an
 * IF whose inner IF has no else, which no parse produces) is wrapped in begin/end.
 * INVALID_ARG (logged) for a NULL argument, a root that is 0 or not made, or a node of an
 * unknown kind; NO_MEMORY on out of memory. On failure out's content is unspecified.
 */
odin3_status odin3_ast_print(const odin3_ast *ast, odin3_ast_id root, odin3_strbuf *out);

/*
 * Appends the subtree of root as an s-expression to out, one node per line, 2-space indent per
 * level: `(KIND[.sub][:name] @file:line:col[ payload] children…)` — sub the sub-kind spelling,
 * name the name bytes (a STRING's as a C string), the location through the source manager
 * (`<unknown>:0:0` for 0), payload a NUMBER or REAL as printed Verilog and a TEXT as a C string;
 * `_` for an absent slot; the node's attributes after its children as `{(ATTR:…) (ATTR:…)}` (the
 * first after `{`, the rest one column right of it, each on its own line). Flags are not shown.
 * Indentation stops growing at 2 · ODIN3_PR_MAX_INDENT columns (a 32768-high chain would
 * otherwise need a gigabyte of blanks).
 * INVALID_ARG (logged) for a NULL argument or a root that is 0 or not made; NO_MEMORY. On failure
 * out's content is unspecified.
 */
odin3_status odin3_ast_dump(const odin3_ast *ast, odin3_ast_id root, odin3_strbuf *out);

/*
 * The subtrees ra of ast_a and rb of ast_b (the same or different stores and designs) are equal:
 * per node pair the same kind, sub, flags, name bytes, payload value (NUMBER width, nbits, bits,
 * has_xz, has_question; REAL value; TEXT bytes), child count with absent slots in the same
 * places, and attribute spans of equal ATTR nodes; locations, comments and `warned` are
 * ignored. *equal gets the answer. INVALID_ARG (logged) for a NULL argument or a root that is 0
 * or not made; NO_MEMORY (*equal unchanged).
 */
odin3_status odin3_ast_equal(const odin3_ast *ast_a, odin3_ast_id ra, const odin3_ast *ast_b,
                             odin3_ast_id rb, bool *equal);

/* --- internals: shared by print.c, print_expr.c, print_stmt.c and dump.c ---------------------- */

/* Indent levels beyond which the printer and the dump stop indenting further. */
enum { ODIN3_PR_MAX_INDENT = 128 };

/* How a node is printed in its context. */
typedef enum odin3_pr_mode {
    ODIN3_PR_EXPR = 0, /* a top expression: no parentheses */
    ODIN3_PR_OPERAND,  /* an operand: parenthesised when UNARY/BINARY/TERNARY */
    ODIN3_PR_ITEM,     /* an item or statement on a fresh line at its indent, newline-ended */
    ODIN3_PR_INLINE,   /* a statement continuing the current line, newline-ended */
    ODIN3_PR_LISTED    /* inside a list or header: no indent, no `;`, no newline */
} odin3_pr_mode;

/* What a task does when it is popped. */
typedef enum odin3_pr_op {
    ODIN3_PR_OP_NODE = 0, /* print node in mode at indent */
    ODIN3_PR_OP_TEXT,     /* append text (static) */
    ODIN3_PR_OP_NAME,     /* append node's name as an identifier (escaped when needed) */
    ODIN3_PR_OP_RAW,      /* append node's name bytes as stored */
    ODIN3_PR_OP_INDENT,   /* append 2 · indent blanks */
    ODIN3_PR_OP_LEAF      /* append node's payload or STRING as Verilog (TEXT verbatim) */
} odin3_pr_op;

typedef struct odin3_pr_task {
    const char *text;
    uint32_t node;
    uint32_t indent;
    uint8_t op;   /* odin3_pr_op */
    uint8_t mode; /* odin3_pr_mode */
} odin3_pr_task;

/* The printer's state: the pending tasks and the tasks a handler is producing. */
typedef struct odin3_printer {
    const odin3_ast *ast;
    odin3_strbuf *out;
    odin3_vec stack; /* odin3_pr_task, popped from the end */
    odin3_vec seq;   /* odin3_pr_task, the current handler's output in print order */
    odin3_status st; /* the first failure */
} odin3_printer;

/* Prints the node of task (pushing its parts through the odin3_pr_* calls below). */
typedef void (*odin3_pr_handler)(odin3_printer *pr, const odin3_pr_task *task);

/* The handler of an expression kind (print_expr.c) or of any other kind (print_stmt.c). */
odin3_pr_handler odin3_pr_expr_handler(uint32_t kind);
odin3_pr_handler odin3_pr_stmt_handler(uint32_t kind);

/* Handler output, in print order (each records NO_MEMORY in pr->st on failure). */
void odin3_pr_text(odin3_printer *pr, const char *text);
void odin3_pr_node(odin3_printer *pr, odin3_ast_id node, odin3_pr_mode mode, uint32_t indent);
void odin3_pr_name(odin3_printer *pr, odin3_ast_id node);
void odin3_pr_raw(odin3_printer *pr, odin3_ast_id node);
void odin3_pr_indent(odin3_printer *pr, uint32_t indent);
void odin3_pr_leaf(odin3_printer *pr, odin3_ast_id node);

/* The `(* … *) ` group of node's attributes (METACOMMENT ones too unless skip_meta). */
void odin3_pr_attrs(odin3_printer *pr, odin3_ast_id node, bool skip_meta);

/* Children start..n of node as `node, node, …` in mode (0 entries print empty). */
void odin3_pr_list(odin3_printer *pr, odin3_pr_mode mode, odin3_ast_id node, uint32_t start);

/* Appends bytes as a C string literal: `"` … `"` with `\n \t \\ \"` and `\ooo` escapes. */
odin3_status odin3_pr_append_cstring(odin3_strbuf *out, odin3_bytes bytes);

/* Appends a NUMBER node's literal (print_expr.c). INVALID_ARG for a non-NUMBER node. */
odin3_status odin3_pr_append_number(odin3_strbuf *out, const odin3_ast *ast, odin3_ast_id node);

/* Appends value as `%.17g`, with `.0` when that alone would read as an integer. */
odin3_status odin3_pr_append_real(odin3_strbuf *out, double value);

#endif
