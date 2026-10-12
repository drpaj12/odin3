/*
 * elab_test_tree.h — static helpers that build expression trees with the 2A builders, for the 2D
 * unit tests that run without the parser (2C): parsed-form trees (et_*), and whole elaborated
 * stores (eb_*, 2D Task 13: the binder's hand-built stores, the slang seam). Each node is made at
 * the tree's current location (et_line, et_col) in a FILE buffer of ET_LINE_BYTES-byte lines (one
 * line for et_init, ET_LINES for eb_init), so a located diagnostic reads "<file>:<line>:<col>: …".
 * Every helper asserts (Unity) that the builder succeeded.
 */
#ifndef ODIN3_TESTS_UNIT_ELAB_TEST_TREE_H
#define ODIN3_TESTS_UNIT_ELAB_TEST_TREE_H

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "unity.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    ET_LINE_BYTES = 256, /* one line of the FILE buffer */
    ET_LINES = 64,       /* eb_init's buffer: lines 1 .. ET_LINES */
    ET_INT_BITS = 32,    /* eb_int's NUMBER */
};

/* A store and the location its next node gets. */
typedef struct et_tree {
    odin3_design *design; /* borrowed */
    odin3_ast *ast;       /* owned: et_free */
    odin3_srcman *sm;     /* the design's */
    odin3_srcbuf_id buf;  /* the FILE buffer */
    odin3_loc at;         /* the loc of the next node */
    uint32_t line;        /* the current line (et_line) */
    uint32_t lines;       /* lines in the buffer */
} et_tree;

/* A store of form over a FILE buffer named file of `lines` lines; nodes start at line 1, column
 * 1. */
typedef struct et_store {
    odin3_ast_form form;
    uint32_t lines;
} et_store;

static inline void et_open(et_tree *tree, odin3_design *design, const char *file, et_store store) {
    *tree = (et_tree){.design = design, .line = 1, .lines = store.lines};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_ast_create(design, store.form, (odin3_passrun_id){1}, &tree->ast));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_srcman(design, &tree->sm));
    uint32_t name = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(tree->ast, odin3_bytes_cstr(file), &name));
    odin3_srcfile_spec spec = {.name = name, .len = ET_LINE_BYTES * store.lines};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_srcman_add_file(tree->sm, &spec, &tree->buf));
    for (uint32_t i = 1; i < store.lines; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_srcman_add_line(tree->sm, tree->buf, i * ET_LINE_BYTES));
    }
    tree->at = odin3_srcman_loc(tree->sm, tree->buf, 0);
}

/* A new parsed store in design over a one-line FILE buffer named file. */
static inline void et_init(et_tree *tree, odin3_design *design, const char *file) {
    et_open(tree, design, file, (et_store){ODIN3_AST_FORM_PARSED, 1});
}

/* Destroys the store. */
static inline void et_free(et_tree *tree) {
    odin3_ast_destroy(tree->ast);
    tree->ast = NULL;
}

/* The next nodes are made at the current line, column col (1 .. ET_LINE_BYTES). */
static inline void et_col(et_tree *tree, uint32_t col) {
    TEST_ASSERT_TRUE(col >= 1 && col <= ET_LINE_BYTES);
    tree->at = odin3_srcman_loc(tree->sm, tree->buf, (tree->line - 1) * ET_LINE_BYTES + col - 1);
}

/* The next nodes are made at line `line` (1 .. the buffer's lines), column 1. */
static inline void et_line(et_tree *tree, uint32_t line) {
    TEST_ASSERT_TRUE(line >= 1 && line <= tree->lines);
    tree->line = line;
    et_col(tree, 1);
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

/* --- elaborated stores (eb_*, 2D Task 13) ------------------------------------------------ */

/* A new ELABORATED store in design over a FILE buffer named file of ET_LINES lines. */
static inline void eb_init(et_tree *tree, odin3_design *design, const char *file) {
    et_open(tree, design, file, (et_store){ODIN3_AST_FORM_ELABORATED, ET_LINES});
}

/* A strtab ID of the tree's design. */
static inline uint32_t eb_intern(et_tree *tree, const char *name) {
    uint32_t id = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_intern(tree->ast, odin3_bytes_cstr(name), &id));
    return id;
}

/* A node of kind named name (NULL: unnamed) with sub, flags and children kids[0 .. n). */
typedef struct eb_shape {
    uint8_t kind;
    uint8_t sub;
    uint16_t flags;
    const char *name;
} eb_shape;

static inline odin3_ast_id eb_node(et_tree *tree, eb_shape shape, const odin3_ast_id *kids,
                                   uint32_t n) {
    odin3_ast_spec spec = {.kind = shape.kind, .sub = shape.sub, .flags = shape.flags};
    spec.name = shape.name != NULL ? eb_intern(tree, shape.name) : 0;
    return et_node(tree, spec, kids, n);
}

/* A NUMBER in plan point (bb)'s 32-bit integer form: sub DEC, SIGNED, payload width 32. */
static inline odin3_ast_id eb_int(et_tree *tree, int32_t value) {
    uint8_t bits[ET_INT_BITS];
    for (uint32_t i = 0; i < ET_INT_BITS; i++) {
        bits[i] = (((uint32_t)value >> i) & 1U) != 0 ? (uint8_t)ODIN3_BIT_1 : (uint8_t)ODIN3_BIT_0;
    }
    odin3_ast_number num = {.width = ET_INT_BITS, .nbits = ET_INT_BITS, .bits = bits};
    odin3_ast_spec spec = {
        .kind = ODIN3_AST_NUMBER, .sub = ODIN3_AST_NUMBER_DEC, .flags = ODIN3_AST_F_NUMBER_SIGNED};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_number_new(tree->ast, tree->at, &num, &spec.payload));
    return et_node(tree, spec, NULL, 0);
}

/* A RANGE's bounds (any constant nodes). */
typedef struct eb_bounds {
    odin3_ast_id msb;
    odin3_ast_id lsb;
} eb_bounds;

static inline odin3_ast_id eb_range(et_tree *tree, eb_bounds bounds) {
    const odin3_ast_id kids[] = {bounds.msb, bounds.lsb};
    return eb_node(tree, (eb_shape){ODIN3_AST_RANGE, 0, 0, NULL}, kids, 2);
}

static inline odin3_ast_id eb_list(et_tree *tree, const odin3_ast_id *items, uint32_t n) {
    return eb_node(tree, (eb_shape){ODIN3_AST_LIST, 0, 0, NULL}, items, n);
}

/* One declaration with one declarator: the declaration's sub-kind and flags, its packed RANGE (0:
 * none), the declarator's name, its E (0: none) and its unpacked RANGEs. */
typedef struct eb_decl {
    uint8_t sub;
    uint16_t flags;
    odin3_ast_id range;
    const char *name;
    odin3_ast_id value;
    const odin3_ast_id *dims;
    uint32_t n_dims;
} eb_decl;

static inline odin3_ast_id eb_declarator(et_tree *tree, const eb_decl *decl) {
    odin3_ast_id kids[1 + ET_LINES] = {decl->value};
    TEST_ASSERT_TRUE(decl->n_dims <= ET_LINES);
    for (uint32_t i = 0; i < decl->n_dims; i++) {
        kids[1 + i] = decl->dims[i];
    }
    return eb_node(tree, (eb_shape){ODIN3_AST_DECLARATOR, 0, 0, decl->name}, kids,
                   1 + decl->n_dims);
}

/* NET_DECL[sub]{flags} (no strength or delay). */
static inline odin3_ast_id eb_net(et_tree *tree, const eb_decl *decl) {
    const odin3_ast_id kids[] = {{0}, decl->range, {0}, eb_declarator(tree, decl)};
    return eb_node(tree, (eb_shape){ODIN3_AST_NET_DECL, decl->sub, decl->flags, NULL}, kids, 4);
}

/* VAR_DECL[sub]{flags}. */
static inline odin3_ast_id eb_var(et_tree *tree, const eb_decl *decl) {
    const odin3_ast_id kids[] = {decl->range, eb_declarator(tree, decl)};
    return eb_node(tree, (eb_shape){ODIN3_AST_VAR_DECL, decl->sub, decl->flags, NULL}, kids, 2);
}

/* PORT_DECL[sub = direction]{flags | IN_HEADER} (flags carry DT, NET and SIGNED). */
static inline odin3_ast_id eb_port(et_tree *tree, const eb_decl *decl) {
    const odin3_ast_id kids[] = {decl->range, eb_declarator(tree, decl)};
    uint16_t flags = (uint16_t)(decl->flags | ODIN3_AST_F_PORT_DECL_IN_HEADER);
    return eb_node(tree, (eb_shape){ODIN3_AST_PORT_DECL, decl->sub, flags, NULL}, kids, 2);
}

/* The PORT_DECL flags of a `wire` port. */
static inline uint16_t eb_wire_port(void) {
    return (uint16_t)((1U + (uint32_t)ODIN3_AST_NET_DECL_WIRE) << ODIN3_AST_F_PORT_DECL_NET_SHIFT);
}

/* PARAM_DECL[localparam] (no range) with one declarator name = value. */
static inline odin3_ast_id eb_localparam(et_tree *tree, const char *name, odin3_ast_id value) {
    eb_decl decl = {.name = name, .value = value};
    const odin3_ast_id kids[] = {{0}, eb_declarator(tree, &decl)};
    return eb_node(tree, (eb_shape){ODIN3_AST_PARAM_DECL, ODIN3_AST_PARAM_DECL_LOCALPARAM, 0, NULL},
                   kids, 2);
}

/* A MODULE: its params and ports LISTs (0: none) and items. */
typedef struct eb_mod {
    const char *name;
    odin3_ast_id params;
    odin3_ast_id ports;
    const odin3_ast_id *items;
    uint32_t n;
} eb_mod;

static inline odin3_ast_id eb_module(et_tree *tree, const eb_mod *mod) {
    odin3_ast_id kids[2 + ET_LINE_BYTES] = {mod->params, mod->ports};
    TEST_ASSERT_TRUE(mod->n <= ET_LINE_BYTES);
    for (uint32_t i = 0; i < mod->n; i++) {
        kids[2 + i] = mod->items[i];
    }
    return eb_node(tree, (eb_shape){ODIN3_AST_MODULE, 0, 0, mod->name}, kids, 2 + mod->n);
}

/* The UNIT of modules[0 .. n), named after modules[0] (the top); the store is then finished. */
static inline odin3_ast_id eb_unit(et_tree *tree, const odin3_ast_id *modules, uint32_t n) {
    odin3_ast_spec spec = {.kind = ODIN3_AST_UNIT, .name = odin3_ast_name(tree->ast, modules[0])};
    odin3_ast_id unit = et_node(tree, spec, modules, n);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_finish(tree->ast));
    return unit;
}

/* A GEN_BLOCK (slot: its index NUMBER or 0) or a SEQ_BLOCK (name NULL: unnamed; slot: its locals
 * LIST or 0) with items. */
typedef struct eb_block {
    const char *name;
    odin3_ast_id slot;
    const odin3_ast_id *items;
    uint32_t n;
} eb_block;

static inline odin3_ast_id eb_block_of(et_tree *tree, uint8_t kind, const eb_block *blk) {
    odin3_ast_id kids[1 + ET_LINE_BYTES] = {blk->slot};
    TEST_ASSERT_TRUE(blk->n <= ET_LINE_BYTES);
    for (uint32_t i = 0; i < blk->n; i++) {
        kids[1 + i] = blk->items[i];
    }
    return eb_node(tree, (eb_shape){kind, 0, 0, blk->name}, kids, 1 + blk->n);
}

static inline odin3_ast_id eb_gen_block(et_tree *tree, const eb_block *blk) {
    return eb_block_of(tree, ODIN3_AST_GEN_BLOCK, blk);
}

static inline odin3_ast_id eb_named_block(et_tree *tree, const eb_block *blk) {
    return eb_block_of(tree, ODIN3_AST_SEQ_BLOCK, blk);
}

/* A FUNCTION_DECL (range: its return RANGE or 0) or TASK_DECL; ports and locals LISTs (0: empty
 * ones) and body (0: a NULL_STMT). */
typedef struct eb_subr {
    const char *name;
    uint16_t flags;
    odin3_ast_id range;
    odin3_ast_id ports;
    odin3_ast_id locals;
    odin3_ast_id body;
} eb_subr;

static inline odin3_ast_id eb_subr_of(et_tree *tree, uint8_t kind, const eb_subr *subr) {
    odin3_ast_id ports = subr->ports.v != 0 ? subr->ports : eb_list(tree, NULL, 0);
    odin3_ast_id locals = subr->locals.v != 0 ? subr->locals : eb_list(tree, NULL, 0);
    odin3_ast_id body = subr->body.v != 0
                            ? subr->body
                            : eb_node(tree, (eb_shape){ODIN3_AST_NULL_STMT, 0, 0, NULL}, NULL, 0);
    const odin3_ast_id function_kids[] = {subr->range, ports, locals, body};
    bool is_function = kind == ODIN3_AST_FUNCTION_DECL;
    return eb_node(tree, (eb_shape){kind, 0, subr->flags, subr->name},
                   is_function ? function_kids : function_kids + 1, is_function ? 4 : 3);
}

static inline odin3_ast_id eb_function(et_tree *tree, const eb_subr *subr) {
    return eb_subr_of(tree, ODIN3_AST_FUNCTION_DECL, subr);
}

static inline odin3_ast_id eb_task(et_tree *tree, const eb_subr *subr) {
    return eb_subr_of(tree, ODIN3_AST_TASK_DECL, subr);
}

/* lhs = rhs as kind: ODIN3_AST_CONT_ASSIGN (a CONT_ASSIGN of one NET_ASSIGN), BLOCKING_ASSIGN or
 * NONBLOCKING_ASSIGN (no intra-assignment control). */
static inline odin3_ast_id eb_assign(et_tree *tree, uint8_t kind, odin3_ast_id lhs,
                                     odin3_ast_id rhs) {
    if (kind == ODIN3_AST_CONT_ASSIGN) {
        const odin3_ast_id pair[] = {lhs, rhs};
        const odin3_ast_id kids[] = {
            {0}, {0}, eb_node(tree, (eb_shape){ODIN3_AST_NET_ASSIGN, 0, 0, NULL}, pair, 2)};
        return eb_node(tree, (eb_shape){ODIN3_AST_CONT_ASSIGN, 0, 0, NULL}, kids, 3);
    }
    const odin3_ast_id kids[] = {lhs, {0}, rhs};
    return eb_node(tree, (eb_shape){kind, 0, 0, NULL}, kids, 3);
}

/* EVENT_EXPR[edge](expr) (edge: ODIN3_AST_EVENT_EXPR_*). */
static inline odin3_ast_id eb_event(et_tree *tree, uint8_t edge, odin3_ast_id expr) {
    return eb_node(tree, (eb_shape){ODIN3_AST_EVENT_EXPR, edge, 0, NULL}, &expr, 1);
}

/* EVENT_CONTROL of events[0 .. n) (n = 0: @*). */
static inline odin3_ast_id eb_control(et_tree *tree, const odin3_ast_id *events, uint32_t n) {
    odin3_ast_id kids[1 + ET_LINES] = {{0}};
    TEST_ASSERT_TRUE(n <= ET_LINES);
    for (uint32_t i = 0; i < n; i++) {
        kids[1 + i] = events[i];
    }
    uint16_t flags = n == 0 ? (uint16_t)ODIN3_AST_F_EVENT_CONTROL_STAR : 0;
    return eb_node(tree, (eb_shape){ODIN3_AST_EVENT_CONTROL, 0, flags, NULL}, kids, 1 + n);
}

/* TIMING_STMT[control, body]. */
static inline odin3_ast_id eb_timing(et_tree *tree, odin3_ast_id control, odin3_ast_id body) {
    const odin3_ast_id kids[] = {control, body};
    return eb_node(tree, (eb_shape){ODIN3_AST_TIMING_STMT, 0, 0, NULL}, kids, 2);
}

/* always @(events[0 .. n)) body (n = 0: @*). */
static inline odin3_ast_id eb_always(et_tree *tree, const odin3_ast_id *events, uint32_t n,
                                     odin3_ast_id body) {
    odin3_ast_id timing = eb_timing(tree, eb_control(tree, events, n), body);
    return eb_node(tree, (eb_shape){ODIN3_AST_ALWAYS, ODIN3_AST_ALWAYS_ALWAYS, 0, NULL}, &timing,
                   1);
}

static inline odin3_ast_id eb_initial(et_tree *tree, odin3_ast_id body) {
    return eb_node(tree, (eb_shape){ODIN3_AST_INITIAL, 0, 0, NULL}, &body, 1);
}

/* An unnamed SEQ_BLOCK without locals: begin stmts[0 .. n) end. */
static inline odin3_ast_id eb_begin(et_tree *tree, const odin3_ast_id *stmts, uint32_t n) {
    eb_block blk = {NULL, {0}, stmts, n};
    return eb_named_block(tree, &blk);
}

/* A named CONNECTION .port(expr) (port NULL: ordered; expr 0: unconnected). */
static inline odin3_ast_id eb_conn(et_tree *tree, const char *port, odin3_ast_id expr) {
    return eb_node(tree, (eb_shape){ODIN3_AST_CONNECTION, 0, 0, port}, &expr, 1);
}

/* An INSTANTIATION of module (its params LIST or 0) with one INSTANCE name (no range) and its
 * connections. */
typedef struct eb_inst {
    const char *module;
    const char *name;
    odin3_ast_id params;
    const odin3_ast_id *conns;
    uint32_t n;
} eb_inst;

static inline odin3_ast_id eb_instance(et_tree *tree, const eb_inst *inst) {
    odin3_ast_id kids[1 + ET_LINES] = {{0}};
    TEST_ASSERT_TRUE(inst->n <= ET_LINES);
    for (uint32_t i = 0; i < inst->n; i++) {
        kids[1 + i] = inst->conns[i];
    }
    const odin3_ast_id holder[] = {
        inst->params,
        eb_node(tree, (eb_shape){ODIN3_AST_INSTANCE, 0, 0, inst->name}, kids, 1 + inst->n)};
    return eb_node(tree, (eb_shape){ODIN3_AST_INSTANTIATION, 0, 0, inst->module}, holder, 2);
}

/* A HIER_NAME of parts[0 .. n) (IDENTs, or eb_index's selects). */
static inline odin3_ast_id eb_hier(et_tree *tree, const odin3_ast_id *parts, uint32_t n) {
    return eb_node(tree, (eb_shape){ODIN3_AST_HIER_NAME, 0, 0, NULL}, parts, n);
}

/* name[index]: a bit SELECT of an IDENT by a 32-bit NUMBER (a HIER_NAME part, or a select). */
static inline odin3_ast_id eb_index(et_tree *tree, const char *name, int32_t index) {
    const et_sel sel = {ODIN3_AST_SELECT_BIT, et_ident(tree, name), eb_int(tree, index), {0}};
    return et_select(tree, sel);
}

/* name(args[0 .. n)) as a TASK_CALL statement; a name starting with '$' is a system task. */
static inline odin3_ast_id eb_task_call(et_tree *tree, const char *name, const odin3_ast_id *args,
                                        uint32_t n) {
    uint16_t flags = name[0] == '$' ? (uint16_t)ODIN3_AST_F_TASK_CALL_SYSTEM : 0;
    return eb_node(tree, (eb_shape){ODIN3_AST_TASK_CALL, 0, flags, name}, args, n);
}

/* Attaches (* name *) (no value) to node. */
static inline void eb_attr(et_tree *tree, odin3_ast_id node, const char *name) {
    odin3_ast_id value = {0};
    odin3_ast_id attr = eb_node(tree, (eb_shape){ODIN3_AST_ATTR, 0, 0, name}, &value, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ast_attach(tree->ast, node, &attr, 1));
}

#endif
