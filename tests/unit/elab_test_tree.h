/*
 * elab_test_tree.h — static helpers that build parsed-form expression trees with the 2A
 * builders, for the 2D unit tests that run without the parser (2C). Each node is made at the
 * tree's current location (et_col), in a one-line FILE buffer, so a located diagnostic reads
 * "<file>:1:<col>: …". Every helper asserts (Unity) that the builder succeeded.
 */
#ifndef ODIN3_TESTS_UNIT_ELAB_TEST_TREE_H
#define ODIN3_TESTS_UNIT_ELAB_TEST_TREE_H

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { ET_LINE_BYTES = 256 /* the FILE buffer: one line */ };

/* A parsed store and the location its next node gets. */
typedef struct et_tree {
    odin3_design *design; /* borrowed */
    odin3_ast *ast;       /* owned: et_free */
    odin3_srcman *sm;     /* the design's */
    odin3_srcbuf_id buf;  /* the one-line FILE buffer */
    odin3_loc at;         /* the loc of the next node */
} et_tree;

/* A new parsed store in design over a FILE buffer named file; nodes start at column 1. */
static inline void et_init(et_tree *tree, odin3_design *design, const char *file) {
    *tree = (et_tree){.design = design};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_create(design, ODIN3_AST_FORM_PARSED,
                                                     (odin3_passrun_id){1}, &tree->ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &tree->sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(tree->ast, odin3_bytes_cstr(file), &name));
    odin3_srcfile_spec spec = {.name = name, .len = ET_LINE_BYTES};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(tree->sm, &spec, &tree->buf));
    tree->at = odin3_srcman_loc(tree->sm, tree->buf, 0);
}

/* Destroys the store. */
static inline void et_free(et_tree *tree) {
    odin3_ast_destroy(tree->ast);
    tree->ast = NULL;
}

/* The next nodes are made at line 1, column col (1 ..). */
static inline void et_col(et_tree *tree, uint32_t col) {
    TEST_ASSERT_TRUE(col >= 1 && col <= ET_LINE_BYTES);
    tree->at = odin3_srcman_loc(tree->sm, tree->buf, col - 1);
}

/* A node of kind with sub, flags and children kids[0 .. n), at the current location. */
static inline odin3_ast_id et_node(et_tree *tree, odin3_ast_spec spec, const odin3_ast_id *kids,
                                   uint32_t n) {
    spec.loc = tree->at.v;
    odin3_ast_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_make(tree->ast, &spec, kids, n, &id));
    return id;
}

/* An IDENT. */
static inline odin3_ast_id et_ident(et_tree *tree, const char *name) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_IDENT};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_intern(tree->ast, odin3_bytes_cstr(name), &spec.name));
    return et_node(tree, spec, NULL, 0);
}

/*
 * A NUMBER from its source text: "8'hFF", "'h1", "4'sd5", "5". The size is what precedes the
 * quote, the base the quote, an optional s and the base letter, the digits the rest; without a
 * quote the text is a plain decimal.
 */
static inline odin3_ast_id et_num(et_tree *tree, const char *text) {
    const char *quote = strchr(text, '\'');
    odin3_number_text pieces = {.size = {0}, .base = {0}, .digits = odin3_bytes_cstr(text)};
    if (quote != NULL) {
        size_t base_len = (quote[1] == 's' || quote[1] == 'S') ? 3 : 2;
        pieces.size = (odin3_bytes){text, (size_t)(quote - text)};
        pieces.base = (odin3_bytes){quote, base_len};
        pieces.digits = odin3_bytes_cstr(quote + base_len);
    }
    odin3_ast_spec spec = {.kind = ODIN3_AST_NUMBER};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_kind(pieces, &spec.sub, &spec.flags));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_number_parse(tree->ast, tree->at, pieces, &spec.payload));
    return et_node(tree, spec, NULL, 0);
}

/* A STRING literal holding text (unescaped bytes). */
static inline odin3_ast_id et_string(et_tree *tree, const char *text) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_STRING};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_intern(tree->ast, odin3_bytes_cstr(text), &spec.name));
    return et_node(tree, spec, NULL, 0);
}

/* A REAL literal. */
static inline odin3_ast_id et_real(et_tree *tree, double value) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_REAL};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_real_new(tree->ast, value, &spec.payload));
    return et_node(tree, spec, NULL, 0);
}

/* A UNARY (sub: ODIN3_AST_UNARY_*). */
static inline odin3_ast_id et_unary(et_tree *tree, uint8_t sub, odin3_ast_id operand) {
    return et_node(tree, (odin3_ast_spec){.kind = ODIN3_AST_UNARY, .sub = sub}, &operand, 1);
}

/* A BINARY (sub: ODIN3_AST_BINARY_*). */
static inline odin3_ast_id et_binary(et_tree *tree, uint8_t sub, odin3_ast_id lhs,
                                     odin3_ast_id rhs) {
    const odin3_ast_id kids[] = {lhs, rhs};
    return et_node(tree, (odin3_ast_spec){.kind = ODIN3_AST_BINARY, .sub = sub}, kids, 2);
}

/* cond ? lhs : rhs. */
static inline odin3_ast_id et_ternary(et_tree *tree, odin3_ast_id cond, odin3_ast_id lhs,
                                      odin3_ast_id rhs) {
    const odin3_ast_id kids[] = {cond, lhs, rhs};
    return et_node(tree, (odin3_ast_spec){.kind = ODIN3_AST_TERNARY}, kids, 3);
}

/* {parts[0], …, parts[n - 1]}. */
static inline odin3_ast_id et_concat(et_tree *tree, const odin3_ast_id *parts, uint32_t n) {
    return et_node(tree, (odin3_ast_spec){.kind = ODIN3_AST_CONCAT}, parts, n);
}

/* {count{body}}: body must be a CONCAT. */
static inline odin3_ast_id et_replicate(et_tree *tree, odin3_ast_id count, odin3_ast_id body) {
    const odin3_ast_id kids[] = {count, body};
    return et_node(tree, (odin3_ast_spec){.kind = ODIN3_AST_REPLICATE}, kids, 2);
}

/*
 * A SELECT: object[index] (BIT), object[index:lsb] (PART), object[index +: lsb] (PART_PLUS) or
 * object[index -: lsb] (PART_MINUS); lsb is 0 (the absent optional slot) for BIT.
 */
typedef struct et_sel {
    uint8_t sub; /* ODIN3_AST_SELECT_* */
    odin3_ast_id object;
    odin3_ast_id index;
    odin3_ast_id lsb;
} et_sel;

static inline odin3_ast_id et_select(et_tree *tree, et_sel sel) {
    const odin3_ast_id kids[] = {sel.object, sel.index, sel.lsb};
    return et_node(tree, (odin3_ast_spec){.kind = ODIN3_AST_SELECT, .sub = sel.sub}, kids, 3);
}

/* name(args[0], …, args[n - 1]); a name starting with '$' is a system call. */
static inline odin3_ast_id et_call(et_tree *tree, const char *name, const odin3_ast_id *args,
                                   uint32_t n) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_CALL};
    spec.flags = name[0] == '$' ? ODIN3_AST_F_CALL_SYSTEM : 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_intern(tree->ast, odin3_bytes_cstr(name), &spec.name));
    return et_node(tree, spec, args, n);
}

#endif
